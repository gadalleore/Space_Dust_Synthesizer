# Modulation Assign Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let any continuous knob in Space Dust V2 be an LFO destination, chosen by pointing at the knob, with four LFOs instead of two and a drawn pitch curve in place of the three Pitch Env knobs.

**Architecture:** A JUCE-free `ModMatrix` core holds an unlimited list of routings and compiles them, off the audio thread, into a flat integer-indexed array the per-sample loop can walk without allocating or touching a string. Voice destinations keep the exact per-sample paths they use today; effect destinations get their per-sample behaviour from running the effects chain in 32-sample chunks, which is only enabled when a patch actually modulates an effect. The routing list and the pitch curve live in the saved patch, not in the parameter list.

**Tech Stack:** C++17, JUCE 8.0.x, CMake, MSVC Release, Visual Studio 17 2022 generator.

**Spec:** [`docs/superpowers/specs/2026-08-28-modulation-assign-mode-design.md`](../specs/2026-08-28-modulation-assign-mode-design.md)

## Global Constraints

- **Never write the string `error :` in test output.** MSBuild reads it as a build failure and fails a `*-test` target with code -1 even when the executable returns 0. Use `FAIL` instead.
- **Any tool that constructs a `SpaceDustAudioProcessor` must use `LiveStateGuard`** (in `tools/unisonaudit/unison_audit_main.cpp`). The processor starts `PresetHotReload` in its constructor and will otherwise overwrite the player's loaded patch in `current.sdpreset`.
- **Never rename a parameter id.** Presets and automation reference ids. Display names may change.
- **New parameters go at the END of `createParameterLayout`.** The VST3 parameter index is an automation contract.
- **No allocation, no locks and no `std::string` on the audio thread.** The compiled routing array exists for this reason.
- **Build and deploy Standalone and VST3 together.** Giuseppe tests from the desktop Standalone; a VST3-only build hands him a stale one.
- Build: `cmake --build build --config Release --target SpaceDust` then `--target SpaceDust_VST3`.
- Configure (first time): `cmake -B build -G "Visual Studio 17 2022" -A x64 -DJUCE_DIR=$env:JUCE_DIR`

---

## File Structure

| File | Responsibility |
| --- | --- |
| `Source/ModMatrix.h` (new) | JUCE-free routing store, compile step, per-sample apply |
| `Source/ModMatrix.cpp` (new) | its implementation |
| `Source/ModMatrixState.h` (new) | JUCE boundary: destination discovery from APVTS, XML save and load |
| `Source/ModMatrixState.cpp` (new) | its implementation |
| `Source/PitchCurve.h` (new) | JUCE-free drawn curve: points, sampling, defaults |
| `Source/PitchCurve.cpp` (new) | its implementation |
| `Source/ModulatableKnob.h` (new) | one knob's ring, drag, percentage readout and indicator bar |
| `Source/ModulatableKnob.cpp` (new) | its implementation |
| `Source/AssignModeState.h` (new) | which LFO is being assigned, and who is told when that changes |
| `tools/modmatrixtest/mod_matrix_test.cpp` (new) | fast JUCE-free unit tests |
| `tools/chunkaudit/chunk_audit_main.cpp` (new) | proves chunked output is bit-identical |
| `Source/PluginProcessor.cpp` | owns the matrix, chunks the effects chain, publishes LFO phase |
| `Source/SynthVoice.cpp` | reads modulated voice values per sample |
| `Source/PluginEditor.cpp` | assign mode, Exit LFO Mode, LFO 3 and 4, pitch curve box |
| `Source/SpaceDustTranceGate.cpp` | takes a sample offset so chunking cannot drift its grid |

---

## Task 1: The ModMatrix core

**Files:**
- Create: `Source/ModMatrix.h`, `Source/ModMatrix.cpp`
- Create: `tools/modmatrixtest/mod_matrix_test.cpp`
- Modify: `CMakeLists.txt` (after the `unison-test` block, around line 472)

**Interfaces:**
- Consumes: nothing.
- Produces: `spacedust::ModRouting`, `spacedust::DestRange`, `spacedust::ModMatrix` with
  `setRouting(int, const std::string&, float)`, `clearRouting(int, const std::string&)`,
  `amountFor(int, const std::string&) const -> float`,
  `hasAnyRouting(const std::string&) const -> bool`,
  `routings() const -> const std::vector<ModRouting>&`, `clear()`,
  `applyByName(const std::string&, float base, DestRange, const float* lfoValues) const -> float`.

Uses `std::string`, not `juce::String`, so the test builds in seconds with no JUCE. `Source/NoteLockGrid.*` is the precedent.

- [ ] **Step 1: Write the failing test**

Create `tools/modmatrixtest/mod_matrix_test.cpp`:

```cpp
// =====================================================================
//  Modulation matrix test
//  ---------------------------------------------------------------------
//  The arithmetic that decides where a knob actually sits once an LFO
//  reaches it. Kept free of JUCE so it builds and runs in seconds.
//
//  Build & run:
//      cmake --build build --config Release --target modmatrix-test
//      ./build/mod_matrix_test/Release/mod_matrix_test.exe
// =====================================================================
#include "../../Source/ModMatrix.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace
{
    int failures = 0;

    void check(bool ok, const std::string& what)
    {
        if (!ok)
        {
            // Never the string "error :" -- MSBuild reads that as a build failure.
            std::printf("  FAIL  %s\n", what.c_str());
            ++failures;
        }
    }

    void checkNear(double got, double want, double tol, const std::string& what)
    {
        if (std::fabs(got - want) > tol)
        {
            std::printf("  FAIL  %s (got %.6f, want %.6f)\n", what.c_str(), got, want);
            ++failures;
        }
    }
}

int main()
{
    using namespace spacedust;

    const DestRange unit { 0.0f, 1.0f };

    // -- an unassigned knob is left exactly where it was --
    {
        ModMatrix m;
        const float lfos[numLfos] = { 1.0f, 1.0f, 1.0f, 1.0f };
        checkNear(m.applyByName("filterCutoff", 0.25f, unit, lfos), 0.25,
                  1e-9, "no routing leaves the base value untouched");
    }

    // -- the LFO rotates around the knob, not around zero --
    {
        ModMatrix m;
        m.setRouting(0, "filterCutoff", 1.0f);
        const float peak[numLfos]   = {  1.0f, 0.0f, 0.0f, 0.0f };
        const float trough[numLfos] = { -1.0f, 0.0f, 0.0f, 0.0f };
        const float centre[numLfos] = {  0.0f, 0.0f, 0.0f, 0.0f };

        // halfRange of 0..1 is 0.5, so +100% at the peak is base + 0.5.
        checkNear(m.applyByName("filterCutoff", 0.5f, unit, peak),   1.0, 1e-6, "peak sits half a range above the knob");
        checkNear(m.applyByName("filterCutoff", 0.5f, unit, trough), 0.0, 1e-6, "trough sits half a range below the knob");
        checkNear(m.applyByName("filterCutoff", 0.5f, unit, centre), 0.5, 1e-6, "the centre of the cycle is the knob itself");

        // Move the knob and the whole movement moves with it.
        checkNear(m.applyByName("filterCutoff", 0.3f, unit, centre), 0.3, 1e-6, "the movement follows the knob");
    }

    // -- a negative amount turns the movement upside down --
    {
        ModMatrix m;
        m.setRouting(0, "filterCutoff", -1.0f);
        const float peak[numLfos] = { 1.0f, 0.0f, 0.0f, 0.0f };
        checkNear(m.applyByName("filterCutoff", 0.5f, unit, peak), 0.0, 1e-6,
                  "a negative amount inverts the movement");
    }

    // -- the result never leaves the knob's legal range --
    {
        ModMatrix m;
        m.setRouting(0, "filterCutoff", 1.0f);
        const float peak[numLfos]   = {  1.0f, 0.0f, 0.0f, 0.0f };
        const float trough[numLfos] = { -1.0f, 0.0f, 0.0f, 0.0f };
        checkNear(m.applyByName("filterCutoff", 0.9f, unit, peak),   1.0, 1e-6, "clamped at the top");
        checkNear(m.applyByName("filterCutoff", 0.1f, unit, trough), 0.0, 1e-6, "clamped at the bottom");
    }

    // -- two LFOs on one knob add before the clamp --
    {
        ModMatrix m;
        m.setRouting(0, "reverbWetMix", 0.5f);
        m.setRouting(1, "reverbWetMix", 0.5f);
        const float both[numLfos] = { 1.0f, 1.0f, 0.0f, 0.0f };
        // 0.5*0.5 + 0.5*0.5 = 0.5 above the knob.
        checkNear(m.applyByName("reverbWetMix", 0.2f, unit, both), 0.7, 1e-6,
                  "two LFOs on one knob add together");
    }

    // -- assigning the same pair twice replaces, it does not stack --
    {
        ModMatrix m;
        m.setRouting(0, "reverbWetMix", 0.25f);
        m.setRouting(0, "reverbWetMix", 0.75f);
        check(m.routings().size() == 1, "assigning the same pair twice keeps one entry");
        checkNear(m.amountFor(0, "reverbWetMix"), 0.75, 1e-6, "the second assignment wins");
    }

    // -- an amount of zero removes the routing rather than leaving a dead entry --
    {
        ModMatrix m;
        m.setRouting(0, "reverbWetMix", 0.5f);
        m.setRouting(0, "reverbWetMix", 0.0f);
        check(m.routings().empty(), "an amount of zero removes the routing");
        check(!m.hasAnyRouting("reverbWetMix"), "and the knob reports no routing");
    }

    // -- clearRouting removes only the pair it names --
    {
        ModMatrix m;
        m.setRouting(0, "a", 0.5f);
        m.setRouting(1, "a", 0.5f);
        m.clearRouting(0, "a");
        check(m.routings().size() == 1, "clearRouting removes one entry");
        check(m.amountFor(1, "a") == 0.5f, "and leaves the other LFO alone");
    }

    // -- an amount outside -1..+1 is clamped when stored --
    {
        ModMatrix m;
        m.setRouting(0, "a",  4.0f);
        m.setRouting(1, "a", -4.0f);
        checkNear(m.amountFor(0, "a"),  1.0, 1e-6, "an amount above +1 is clamped");
        checkNear(m.amountFor(1, "a"), -1.0, 1e-6, "an amount below -1 is clamped");
    }

    // -- an out-of-range LFO index is refused, not stored --
    {
        ModMatrix m;
        m.setRouting(-1, "a", 0.5f);
        m.setRouting(numLfos, "a", 0.5f);
        check(m.routings().empty(), "an illegal LFO index stores nothing");
    }

    // -- a real parameter range, not just 0..1 --
    {
        ModMatrix m;
        m.setRouting(0, "filterCutoff", 1.0f);
        const DestRange hz { 20.0f, 20000.0f };   // halfRange 9990
        const float peak[numLfos] = { 1.0f, 0.0f, 0.0f, 0.0f };
        checkNear(m.applyByName("filterCutoff", 1000.0f, hz, peak), 10990.0, 1e-3,
                  "the amount scales with the knob's own range");
    }

    if (failures == 0)
        std::printf("\nAll modulation matrix tests passed.\n");
    else
        std::printf("\n%d modulation matrix test(s) FAILED.\n", failures);

    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Add the CMake target**

Add to `CMakeLists.txt` immediately after the `unison-test` custom target (around line 472):

```cmake
#==============================================================================
# modmatrix-test — standalone check of the modulation matrix arithmetic.
# Where a knob sits once one or more LFOs reach it: the rotation around the
# knob's own value, the clamp to its legal range, and the rule that two LFOs on
# one knob add before the clamp. No JUCE, so it builds and runs in seconds.
# Usage: cmake --build build --config Release --target modmatrix-test
#==============================================================================
add_executable(mod_matrix_test
    tools/modmatrixtest/mod_matrix_test.cpp
    Source/ModMatrix.cpp)
set_target_properties(mod_matrix_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mod_matrix_test")
target_compile_features(mod_matrix_test PRIVATE cxx_std_17)

add_custom_target(modmatrix-test
    COMMAND $<TARGET_FILE:mod_matrix_test>
    DEPENDS mod_matrix_test
    COMMENT "Running modulation matrix tests")
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
cmake --build build --config Release --target modmatrix-test
```

Expected: FAILS to compile — `Source/ModMatrix.h` does not exist.

- [ ] **Step 4: Write `Source/ModMatrix.h`**

```cpp
#pragma once

#include <string>
#include <vector>

/** The modulation matrix.

    Deliberately free of JUCE so its arithmetic can be tested in seconds without
    building a plugin (Source/NoteLockGrid.* is the same idea). The JUCE side --
    finding which parameters may be destinations, and saving the list into the
    patch -- lives in ModMatrixState.h.

    There is no limit on how many routings exist. That is why the list is saved
    in the patch rather than exposed as parameters: one parameter per possible
    pair would be four LFOs times about 150 knobs. */
namespace spacedust
{
    inline constexpr int numLfos = 4;

    /** One LFO reaching one knob. */
    struct ModRouting
    {
        int         lfoIndex = 0;    // 0..numLfos-1
        std::string destination;     // an APVTS parameter id
        float       amount = 0.0f;   // -1..+1
    };

    /** A destination knob's legal range, as the APVTS reports it. */
    struct DestRange
    {
        float start = 0.0f;
        float end   = 1.0f;

        float halfRange() const noexcept { return (end - start) * 0.5f; }
    };

    /** A routing with the strings and the lookups already taken out of it.

        The audio thread walks these, never the ModRouting list: no string
        compare, no allocation, and the scale is pre-multiplied. destSlot is an
        index into whatever array of destinations the caller compiled against. */
    struct CompiledRouting
    {
        int   destSlot = 0;
        int   lfoIndex = 0;
        float scale    = 0.0f;   // amount * range.halfRange()
    };

    class ModMatrix
    {
    public:
        //======================================================================
        // -- Editing. Message thread only. --

        /** Set how far one LFO moves one knob.

            An amount of zero REMOVES the routing rather than leaving a dead
            entry, so a drag back through the middle undoes the assignment. An
            illegal LFO index is refused. Assigning a pair that already exists
            replaces its amount. */
        void setRouting (int lfoIndex, const std::string& destination, float amount);

        /** Remove one LFO from one knob, leaving any other LFO on it alone. */
        void clearRouting (int lfoIndex, const std::string& destination);

        void clear();

        //======================================================================
        // -- Reading --

        float amountFor (int lfoIndex, const std::string& destination) const;

        /** Whether any LFO at all reaches this knob. This is what decides
            whether the indicator bar is drawn beside it. */
        bool hasAnyRouting (const std::string& destination) const;

        const std::vector<ModRouting>& routings() const noexcept { return list; }

        //======================================================================
        // -- The value --

        /** Where the knob actually sits.

            lfoValues points at numLfos floats, each -1..+1 and ALREADY scaled by
            that LFO's own Depth knob. The amount here is a further trim, so
            Depth is the master level for one LFO and the amount is the balance
            between the knobs it reaches. Nothing may apply Depth twice.

            Convenience for tests and for the editor. The audio thread uses the
            compiled form instead -- this one compares strings. */
        float applyByName (const std::string& destination, float base,
                           DestRange range, const float* lfoValues) const noexcept;

        /** The same arithmetic, for one already-compiled destination.

            out is indexed by destSlot. bases and ranges are too. Walks the
            compiled list once, adds every contribution, then clamps. */
        static void applyCompiled (const CompiledRouting* compiled, int numCompiled,
                                   const float* bases, const DestRange* ranges,
                                   int numDests, const float* lfoValues,
                                   float* out) noexcept;

    private:
        std::vector<ModRouting> list;

        const ModRouting* find (int lfoIndex, const std::string& destination) const;
    };
}
```

- [ ] **Step 5: Write `Source/ModMatrix.cpp`**

```cpp
#include "ModMatrix.h"

#include <algorithm>
#include <cmath>

namespace
{
    float clampf (float v, float lo, float hi) noexcept
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }
}

namespace spacedust
{
    const ModRouting* ModMatrix::find (int lfoIndex, const std::string& destination) const
    {
        for (const auto& r : list)
            if (r.lfoIndex == lfoIndex && r.destination == destination)
                return &r;

        return nullptr;
    }

    void ModMatrix::setRouting (int lfoIndex, const std::string& destination, float amount)
    {
        if (lfoIndex < 0 || lfoIndex >= numLfos || destination.empty())
            return;

        const float a = clampf (amount, -1.0f, 1.0f);

        // Zero means "not assigned". Storing it would leave an entry that draws
        // an indicator bar and moves nothing.
        if (a == 0.0f)
        {
            clearRouting (lfoIndex, destination);
            return;
        }

        for (auto& r : list)
        {
            if (r.lfoIndex == lfoIndex && r.destination == destination)
            {
                r.amount = a;
                return;
            }
        }

        list.push_back (ModRouting { lfoIndex, destination, a });
    }

    void ModMatrix::clearRouting (int lfoIndex, const std::string& destination)
    {
        list.erase (std::remove_if (list.begin(), list.end(),
                                    [&] (const ModRouting& r)
                                    {
                                        return r.lfoIndex == lfoIndex
                                            && r.destination == destination;
                                    }),
                    list.end());
    }

    void ModMatrix::clear()
    {
        list.clear();
    }

    float ModMatrix::amountFor (int lfoIndex, const std::string& destination) const
    {
        if (const auto* r = find (lfoIndex, destination))
            return r->amount;

        return 0.0f;
    }

    bool ModMatrix::hasAnyRouting (const std::string& destination) const
    {
        for (const auto& r : list)
            if (r.destination == destination)
                return true;

        return false;
    }

    float ModMatrix::applyByName (const std::string& destination, float base,
                                  DestRange range, const float* lfoValues) const noexcept
    {
        float sum = base;

        for (const auto& r : list)
        {
            if (r.destination != destination)
                continue;

            if (r.lfoIndex < 0 || r.lfoIndex >= numLfos)
                continue;

            sum += r.amount * lfoValues[r.lfoIndex] * range.halfRange();
        }

        return clampf (sum, range.start, range.end);
    }

    void ModMatrix::applyCompiled (const CompiledRouting* compiled, int numCompiled,
                                   const float* bases, const DestRange* ranges,
                                   int numDests, const float* lfoValues,
                                   float* out) noexcept
    {
        for (int i = 0; i < numDests; ++i)
            out[i] = bases[i];

        for (int i = 0; i < numCompiled; ++i)
        {
            const auto& c = compiled[i];

            if (c.destSlot < 0 || c.destSlot >= numDests)
                continue;

            out[c.destSlot] += c.scale * lfoValues[c.lfoIndex];
        }

        for (int i = 0; i < numDests; ++i)
            out[i] = clampf (out[i], ranges[i].start, ranges[i].end);
    }
}
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
cmake --build build --config Release --target modmatrix-test
```

Expected: `All modulation matrix tests passed.`

- [ ] **Step 7: Commit**

```bash
git add Source/ModMatrix.h Source/ModMatrix.cpp tools/modmatrixtest/mod_matrix_test.cpp CMakeLists.txt
git commit -m "feat(mod): the matrix that decides where a modulated knob sits"
```

---

## Task 2: Destinations and saving

**Files:**
- Create: `Source/ModMatrixState.h`, `Source/ModMatrixState.cpp`
- Modify: `CMakeLists.txt` (add the two files to the plugin's `target_sources`)
- Modify: `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp:3235-3335`

**Interfaces:**
- Consumes: `spacedust::ModMatrix`, `spacedust::ModRouting`, `spacedust::DestRange` from Task 1.
- Produces:
  - `spacedust::DestinationTable` with `build(juce::AudioProcessorValueTreeState&)`,
    `size() const -> int`, `idAt(int) const -> const std::string&`,
    `slotFor(const std::string&) const -> int` (-1 when absent),
    `rangeAt(int) const -> DestRange`, `paramAt(int) const -> juce::RangedAudioParameter*`
  - `spacedust::toXml(const ModMatrix&) -> std::unique_ptr<juce::XmlElement>` (tag `MODMATRIX`)
  - `spacedust::fromXml(const juce::XmlElement&, ModMatrix&) -> void`

A parameter is a destination when it is an `AudioParameterFloat`, its id does not begin with `lfo`, and it is not `pitchBend`.

- [ ] **Step 1: Write `Source/ModMatrixState.h`**

```cpp
#pragma once

#include "ModMatrix.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <unordered_map>

namespace spacedust
{
    /** Every knob an LFO is allowed to reach, in a fixed order.

        Built once, by walking the APVTS parameter list, so a parameter added
        later becomes assignable with no extra work here. The ORDER is the slot
        order the compiled routings index into, and it is stable for one run of
        the plugin because the parameter list is. It is NOT stable across
        versions, which is why the patch stores parameter ids and not slots. */
    class DestinationTable
    {
    public:
        void build (juce::AudioProcessorValueTreeState& apvts);

        int size() const noexcept { return (int) ids.size(); }

        const std::string& idAt (int slot) const { return ids[(size_t) slot]; }

        /** -1 when this id is not a legal destination, which is what an old
            patch naming a parameter that no longer exists must produce. */
        int slotFor (const std::string& id) const;

        DestRange rangeAt (int slot) const { return ranges[(size_t) slot]; }

        juce::RangedAudioParameter* paramAt (int slot) const { return params[(size_t) slot]; }

        /** Whether this parameter may be modulated at all. Public so the editor
            can decide which knobs light up in assign mode. */
        static bool isLegalDestination (const juce::RangedAudioParameter& p);

    private:
        std::vector<std::string>                 ids;
        std::vector<DestRange>                   ranges;
        std::vector<juce::RangedAudioParameter*> params;
        std::unordered_map<std::string, int>     lookup;
    };

    /** The routing list as a MODMATRIX element, for the saved patch. */
    std::unique_ptr<juce::XmlElement> toXml (const ModMatrix& matrix);

    /** Read a MODMATRIX element back. Replaces whatever the matrix held.
        Entries with an illegal LFO index or an empty id are skipped, so a patch
        from a build with different parameters loads rather than failing. */
    void fromXml (const juce::XmlElement& element, ModMatrix& matrix);
}
```

- [ ] **Step 2: Write `Source/ModMatrixState.cpp`**

```cpp
#include "ModMatrixState.h"

namespace spacedust
{
    bool DestinationTable::isLegalDestination (const juce::RangedAudioParameter& p)
    {
        // Floats only. A wobble between "on" and "off", or between two waveform
        // names, is a different feature and would need a different control.
        if (dynamic_cast<const juce::AudioParameterFloat*> (&p) == nullptr)
            return false;

        const auto id = p.paramID.toStdString();

        // No LFO control may be a destination, including the LFO's own. This is
        // what makes assign mode unnecessary on the Modulation page.
        if (id.rfind ("lfo", 0) == 0)
            return false;

        // The host drives this one.
        if (id == "pitchBend")
            return false;

        return true;
    }

    void DestinationTable::build (juce::AudioProcessorValueTreeState& apvts)
    {
        ids.clear();
        ranges.clear();
        params.clear();
        lookup.clear();

        auto& processor = apvts.processor;

        for (auto* raw : processor.getParameters())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (raw);

            if (ranged == nullptr || ! isLegalDestination (*ranged))
                continue;

            const auto id = ranged->paramID.toStdString();
            const auto r  = ranged->getNormalisableRange();

            lookup.emplace (id, (int) ids.size());
            ids.push_back (id);
            ranges.push_back (DestRange { r.start, r.end });
            params.push_back (ranged);
        }
    }

    int DestinationTable::slotFor (const std::string& id) const
    {
        const auto it = lookup.find (id);
        return it == lookup.end() ? -1 : it->second;
    }

    std::unique_ptr<juce::XmlElement> toXml (const ModMatrix& matrix)
    {
        auto root = std::make_unique<juce::XmlElement> ("MODMATRIX");

        for (const auto& r : matrix.routings())
        {
            auto* e = root->createNewChildElement ("ROUTING");
            e->setAttribute ("lfo", r.lfoIndex);
            e->setAttribute ("dest", juce::String (r.destination));
            e->setAttribute ("amount", (double) r.amount);
        }

        return root;
    }

    void fromXml (const juce::XmlElement& element, ModMatrix& matrix)
    {
        matrix.clear();

        for (auto* e : element.getChildWithTagNameIterator ("ROUTING"))
        {
            const int   lfo    = e->getIntAttribute ("lfo", -1);
            const auto  dest   = e->getStringAttribute ("dest").toStdString();
            const float amount = (float) e->getDoubleAttribute ("amount", 0.0);

            // setRouting refuses an illegal index and an empty id on its own, so
            // a patch saved by a build with different parameters loads rather
            // than failing. The routing it cannot place is simply dropped.
            matrix.setRouting (lfo, dest, amount);
        }
    }
}
```

- [ ] **Step 3: Add the files to the plugin build**

In `CMakeLists.txt`, add to the `SpaceDust` target's `target_sources` list, beside the other `Source/*.cpp` entries:

```cmake
    Source/ModMatrix.cpp
    Source/ModMatrixState.cpp
```

- [ ] **Step 4: Give the processor a matrix**

In `Source/PluginProcessor.h`, add the include and two public members:

```cpp
#include "ModMatrixState.h"
```

```cpp
    /** The routing list, and the knobs it is allowed to reach.

        Public because the editor edits the matrix directly in assign mode.
        Edited on the message thread only; the audio thread reads the compiled
        form that Task 4 publishes, never this. */
    spacedust::ModMatrix        modMatrix;
    spacedust::DestinationTable modDestinations;
```

- [ ] **Step 5: Build the destination table at construction**

In `Source/PluginProcessor.cpp`, at the end of the `SpaceDustAudioProcessor` constructor body (after the existing parameter listener setup, near line 490):

```cpp
    // After the parameters exist, because this walks them.
    modDestinations.build (apvts);
```

- [ ] **Step 6: Save the matrix**

In `getStateInformation` (`PluginProcessor.cpp:3235`), immediately after the `userWaveLibrary` block and before `copyXmlToBinary`:

```cpp
            // The routing list travels with the song. It is not in the parameter
            // list, because one parameter per possible LFO-and-knob pair would be
            // four LFOs times about 150 knobs.
            xml->addChildElement (spacedust::toXml (modMatrix).release());
```

- [ ] **Step 7: Load the matrix**

In `setStateInformation` (`PluginProcessor.cpp:3267`), extend the existing lift-out block. Find the `USERWAVES` lift and add beside it:

```cpp
            // Lifted OUT before the XML becomes the parameter tree, exactly as
            // USERWAVES is. Left in place it would become a child of the APVTS
            // state, be written out again by the next save, and double on every
            // round trip.
            std::unique_ptr<juce::XmlElement> matrixXml;
            if (auto* stored = xmlState->getChildByName ("MODMATRIX"))
            {
                xmlState->removeChildElement (stored, false);
                matrixXml.reset (stored);
            }
```

Then, after `apvts.replaceState (restored);` and beside the `userWaveLibrary` restore:

```cpp
            // A patch with no MODMATRIX is one saved before routings existed.
            // Clearing rather than leaving the last patch's routings in place is
            // what stops one patch's movement leaking into the next.
            if (matrixXml != nullptr)
                spacedust::fromXml (*matrixXml, modMatrix);
            else
                modMatrix.clear();
```

- [ ] **Step 8: Build the plugin**

```bash
cmake --build build --config Release --target SpaceDust
```

Expected: builds clean. Nothing has changed audibly — the matrix is stored and restored but nothing reads it yet.

- [ ] **Step 9: Commit**

```bash
git add Source/ModMatrixState.h Source/ModMatrixState.cpp Source/PluginProcessor.h Source/PluginProcessor.cpp CMakeLists.txt
git commit -m "feat(mod): the routing list travels with the patch"
```

---

## Task 3: Chunk the effects chain, and prove it changes nothing

This task adds no modulation. It only makes the effects chain able to run in pieces, and proves that doing so produces the same samples. Everything in Task 4 rests on this.

**Files:**
- Modify: `Source/SpaceDustTranceGate.h`, `Source/SpaceDustTranceGate.cpp`
- Modify: `Source/PluginProcessor.cpp:2740-2960` (the effects chain)
- Create: `tools/chunkaudit/chunk_audit_main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `SpaceDustAudioProcessor::runEffectsChain(juce::AudioBuffer<float>&, int startSampleInBlock)`, and
  `SpaceDustTranceGate::process(juce::AudioBuffer<float>&, double sampleRate, juce::AudioPlayHead*, int sampleOffset)`.

- [ ] **Step 1: Write the failing audit**

Create `tools/chunkaudit/chunk_audit_main.cpp`:

```cpp
// =====================================================================
//  Chunked effects chain audit
//  ---------------------------------------------------------------------
//  Everything about per-sample modulation of an effect rests on one
//  claim: running the effects chain in 32-sample pieces produces the
//  same samples as running it whole. This renders the same notes both
//  ways and compares them bit for bit.
//
//  Build: cmake -B build -DENABLE_CHUNK_AUDIT=ON
//         cmake --build build --config Release --target SpaceDustChunkAudit
// =====================================================================
#include "PluginProcessor.h"
#include "PresetManager.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace
{
    /** Put the player's loaded patch back after this harness has trampled it.

        SpaceDustAudioProcessor starts PresetHotReload from its own CONSTRUCTOR,
        which publishes live parameter state to current.sdpreset. Merely building
        a processor here overwrites whatever patch the player had loaded. */
    struct LiveStateGuard
    {
        explicit LiveStateGuard (juce::File fileToGuard)
            : file (std::move (fileToGuard))
        {
            had = file.existsAsFile() && file.loadFileAsData (data);
        }

        ~LiveStateGuard()
        {
            if (had)
                file.replaceWithData (data.getData(), data.getSize());
        }

        juce::File       file;
        juce::MemoryBlock data;
        bool             had = false;
    };

    constexpr double sampleRate = 44100.0;
    constexpr int    blockSize  = 512;
    constexpr int    numBlocks  = 40;

    /** Render a fixed two-note phrase through a processor and return every
        sample. forceChunking makes the effects chain run in 32-sample pieces
        even though nothing is modulated. */
    std::vector<float> render (bool forceChunking)
    {
        std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
        auto* sd = dynamic_cast<SpaceDustAudioProcessor*> (proc.get());

        sd->setForceEffectChunkingForTests (forceChunking);

        proc->prepareToPlay (sampleRate, blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        std::vector<float> out;
        out.reserve ((size_t) (numBlocks * blockSize * 2));

        for (int b = 0; b < numBlocks; ++b)
        {
            buffer.clear();
            juce::MidiBuffer midi;

            if (b == 0)
            {
                midi.addEvent (juce::MidiMessage::noteOn  (1, 57, 0.9f), 0);
                midi.addEvent (juce::MidiMessage::noteOn  (1, 64, 0.9f), 8);
            }
            if (b == 30)
            {
                midi.addEvent (juce::MidiMessage::noteOff (1, 57), 0);
                midi.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
            }

            proc->processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    out.push_back (buffer.getSample (ch, i));
        }

        proc->releaseResources();
        return out;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const LiveStateGuard liveState (PresetManager::appDataFolder()
                                        .getChildFile ("current.sdpreset"));

    std::printf ("Chunked effects chain audit\n");
    std::printf ("===========================\n\n");

    const auto whole   = render (false);
    const auto chunked = render (true);

    if (whole.size() != chunked.size())
    {
        std::printf ("  FAIL  sample counts differ (%zu vs %zu)\n",
                     whole.size(), chunked.size());
        return 1;
    }

    size_t differing = 0;
    double worst = 0.0;

    for (size_t i = 0; i < whole.size(); ++i)
    {
        const double d = std::abs ((double) whole[i] - (double) chunked[i]);

        if (d != 0.0)
        {
            ++differing;
            worst = juce::jmax (worst, d);
        }
    }

    std::printf ("  samples compared : %zu\n", whole.size());
    std::printf ("  samples differing: %zu\n", differing);
    std::printf ("  largest gap      : %.12f\n\n", worst);

    if (differing == 0)
    {
        std::printf ("  chunked output is bit-identical to whole-block output.\n");
        return 0;
    }

    // Never the string "error :" -- MSBuild reads that as a build failure.
    std::printf ("  FAIL  chunking changed the audio. The chunking is wrong.\n");
    return 1;
}
```

- [ ] **Step 2: Add the CMake target**

Add to `CMakeLists.txt` after the `ENABLE_UNISON_AUDIT` block (around line 687):

```cmake
#==============================================================================
# Chunked effects chain audit. Renders the same notes with the effects chain run
# whole and run in 32-sample pieces, and compares them bit for bit. Per-sample
# modulation of an effect is only safe if these are identical.
# Build:  cmake -B build -DENABLE_CHUNK_AUDIT=ON
#         cmake --build build --config Release --target SpaceDustChunkAudit
#==============================================================================
option(ENABLE_CHUNK_AUDIT "Build the offline chunked-effects equality audit" OFF)
if(ENABLE_CHUNK_AUDIT)
    juce_add_console_app(SpaceDustChunkAudit PRODUCT_NAME "SpaceDustChunkAudit")
    target_sources(SpaceDustChunkAudit PRIVATE tools/chunkaudit/chunk_audit_main.cpp)
    target_include_directories(SpaceDustChunkAudit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_link_libraries(SpaceDustChunkAudit PRIVATE
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
    message(STATUS "SpaceDustChunkAudit ENABLED.")
endif()
```

- [ ] **Step 3: Run it to verify it fails**

```bash
cmake -B build -DENABLE_CHUNK_AUDIT=ON
cmake --build build --config Release --target SpaceDustChunkAudit
```

Expected: FAILS to compile — `setForceEffectChunkingForTests` does not exist.

- [ ] **Step 4: Give the trance gate a sample offset**

In `Source/SpaceDustTranceGate.h`, change the declaration:

```cpp
    /** sampleOffset is how far into the host's current block this buffer starts.

        The gate works out its own position in the bar from the playhead, and the
        playhead reports the position of the BLOCK. Called on 32-sample chunks
        without this, it would read the same position sixteen times and its grid
        would stand still while the audio moved. */
    void process (juce::AudioBuffer<float>& buffer,
                  double sampleRate,
                  juce::AudioPlayHead* playHead,
                  int sampleOffset = 0);
```

In `Source/SpaceDustTranceGate.cpp`, find where the playhead position is turned into a position in samples or beats, and add the offset to it before it is used. The exact line depends on the existing code; the rule is that the gate's position must advance by `sampleOffset` samples relative to what the playhead reported.

- [ ] **Step 5: Extract the effects chain into one function**

In `Source/PluginProcessor.h`, declare:

```cpp
    /** Every effect, in order, over one buffer.

        startSampleInBlock is how far into the host's block this buffer begins,
        which the trance gate needs so its grid keeps moving when the chain is
        called on chunks.

        Split out of processBlock so it can be called once per block, or sixteen
        times on 32-sample views of the same memory when an effect parameter is
        modulated. */
    void runEffectsChain (juce::AudioBuffer<float>& buffer, int startSampleInBlock);

    /** Test hook: make the chain run chunked even with nothing modulated, so the
        chunk audit can compare the two paths on the same patch. */
    void setForceEffectChunkingForTests (bool shouldChunk) noexcept
    {
        forceChunking = shouldChunk;
    }

private:
    bool forceChunking = false;

    /** 32 samples is a control rate near 1400 Hz at 44.1 kHz -- smooth for any
        LFO you can hear -- and short enough that no effect's own smoothing can
        step audibly between pieces. */
    static constexpr int effectChunkSamples = 32;
```

In `Source/PluginProcessor.cpp`, move the body of the effects section (the calls from `reverb_.process` at line 2753 through `finalEQ_.process` at 2955, with their surrounding enable checks) into `runEffectsChain`, passing `startSampleInBlock` to the trance gate call.

- [ ] **Step 6: Call it chunked or whole**

At the point in `processBlock` where the effects section used to begin:

```cpp
    // Whole block unless something needs the chain finer than that. A patch that
    // modulates no effect parameter costs exactly what it cost before, and
    // produces exactly the same samples -- which tools/chunkaudit proves.
    if (! forceChunking && ! anyEffectParameterIsModulated())
    {
        runEffectsChain (buffer, 0);
    }
    else
    {
        const int total = buffer.getNumSamples();

        for (int start = 0; start < total; start += effectChunkSamples)
        {
            const int len = juce::jmin (effectChunkSamples, total - start);

            // A view onto the same memory, not a copy.
            juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(),
                                            buffer.getNumChannels(),
                                            start,
                                            len);

            runEffectsChain (chunk, start);
        }
    }
```

For this task only, add the stub that Task 4 replaces. **Declare** it in the header
beside `runEffectsChain`:

```cpp
    /** Whether any live routing lands on a parameter the effects chain reads.
        Task 3 stubs this to false; Task 4 gives it the real answer. */
    bool anyEffectParameterIsModulated() const noexcept;
```

and **define** it in the .cpp:

```cpp
bool SpaceDustAudioProcessor::anyEffectParameterIsModulated() const noexcept
{
    // Task 4 replaces this BODY with a real check against the compiled routings.
    // Declared in the header and defined here, never inline, so Task 4 has one
    // definition to replace rather than two to reconcile.
    return false;
}
```

- [ ] **Step 7: Run the audit to verify it passes**

```bash
cmake --build build --config Release --target SpaceDustChunkAudit
./build/SpaceDustChunkAudit_artefacts/Release/SpaceDustChunkAudit.exe
```

Expected: `samples differing: 0` and `chunked output is bit-identical to whole-block output.`

If it is not zero, the chunking is wrong. The usual cause is an effect that reads the playhead or its own block length. Find it before going on — everything after this depends on it.

- [ ] **Step 8: Commit**

```bash
git add Source/PluginProcessor.h Source/PluginProcessor.cpp Source/SpaceDustTranceGate.h Source/SpaceDustTranceGate.cpp tools/chunkaudit/chunk_audit_main.cpp CMakeLists.txt
git commit -m "feat(fx): the effects chain can run in pieces without changing a sample"
```

---

## Task 4: Per-sample delivery

**Files:**
- Modify: `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp:1700-1760` (LFO buffers) and the effects call site
- Modify: `Source/SynthVoice.h`, `Source/SynthVoice.cpp:1699-1750`

**Interfaces:**
- Consumes: `spacedust::ModMatrix::applyCompiled`, `CompiledRouting`, `DestinationTable` (Tasks 1-2); `runEffectsChain` (Task 3).
- Produces:
  - `SpaceDustAudioProcessor::rebuildCompiledRoutings()` — message thread, allocates
  - `SpaceDustAudioProcessor::modulatedValue(int slot) const noexcept -> float`
  - `SpaceDustAudioProcessor::lfoValues[numLfos]` filled per chunk
  - `SpaceDustAudioProcessor::anyEffectParameterIsModulated() const noexcept` — the real one

- [ ] **Step 1: Add the compiled form, double-buffered**

In `Source/PluginProcessor.h`:

```cpp
    /** The routings, with the strings and lookups taken out.

        Two buffers and an atomic index rather than a lock. The message thread
        fills the buffer that is NOT live and then stores its index; the audio
        thread reads whichever index it finds. The audio thread therefore never
        allocates, never compares a string, and never waits. */
    std::vector<spacedust::CompiledRouting> compiledBuffers[2];
    std::atomic<int>                        liveCompiled { 0 };

    /** Base values and ranges, one per destination slot, refreshed once per
        chunk from the parameters. */
    std::vector<float>                  destBases;
    std::vector<spacedust::DestRange>   destRanges;
    std::vector<float>                  destModulated;

    /** Whether any live routing lands on a parameter the effects chain reads.
        Decides whether the chain is chunked at all. */
    std::atomic<bool> effectsAreModulated { false };

    /** Each LFO's current value, already scaled by its Depth. */
    float lfoValues[spacedust::numLfos] { 0.0f, 0.0f, 0.0f, 0.0f };

    /** Rebuild the compiled form from modMatrix. Message thread only --
        it allocates. Call after every change to the routing list. */
    void rebuildCompiledRoutings();
```

`anyEffectParameterIsModulated` is already declared in the header by Task 3. Do not
add a second declaration. Replace its **body** in the .cpp:

```cpp
bool SpaceDustAudioProcessor::anyEffectParameterIsModulated() const noexcept
{
    return effectsAreModulated.load (std::memory_order_relaxed);
}
```

### Step 0: bring the LFO buffers forward first

Steps 4 and 7, and Task 7, all read four LFO buffers. Task 9 adds the LFO 3 and 4
*parameters*, but the *buffers* are needed here. Do this first:

Replace `lfo1Buffer` / `lfo2Buffer` with `juce::AudioBuffer<float> lfoBuffers[spacedust::numLfos]`,
and the paired `lfo1ShState` / `lfo2ShState` and `lfo1SampleHoldValue` /
`lfo2SampleHoldValue` with arrays of the same size. Add:

```cpp
    juce::AudioBuffer<float>& lfoBufferFor (int lfo) noexcept
    {
        return lfoBuffers[juce::jlimit (0, spacedust::numLfos - 1, lfo)];
    }
```

Replace the two copies of the LFO fill code at `PluginProcessor.cpp:1714-1760` with one
loop over all four.

Buffers 2 and 3 stay **silent** until Task 9 adds their parameters: `safeGetParam`
returns 0 for a parameter that does not exist, which gives a depth of 0 and a buffer of
zeros. A zero buffer contributes nothing, so nothing changes audibly here.

- [ ] **Step 2: Implement the rebuild**

In `Source/PluginProcessor.cpp`:

```cpp
void SpaceDustAudioProcessor::rebuildCompiledRoutings()
{
    const int numDests = modDestinations.size();

    destBases.assign ((size_t) numDests, 0.0f);
    destModulated.assign ((size_t) numDests, 0.0f);
    destRanges.resize ((size_t) numDests);

    for (int i = 0; i < numDests; ++i)
        destRanges[(size_t) i] = modDestinations.rangeAt (i);

    // Fill the buffer that is not live, then publish it.
    const int target = 1 - liveCompiled.load (std::memory_order_relaxed);
    auto& out = compiledBuffers[target];
    out.clear();
    out.reserve (modMatrix.routings().size());

    bool touchesEffects = false;

    for (const auto& r : modMatrix.routings())
    {
        const int slot = modDestinations.slotFor (r.destination);

        // An old patch may name a parameter this build does not have.
        if (slot < 0)
            continue;

        const auto range = modDestinations.rangeAt (slot);

        out.push_back (spacedust::CompiledRouting {
            slot, r.lfoIndex, r.amount * range.halfRange() });

        if (isEffectParameter (modDestinations.idAt (slot)))
            touchesEffects = true;
    }

    effectsAreModulated.store (touchesEffects, std::memory_order_relaxed);
    liveCompiled.store (target, std::memory_order_release);
}
```

Add the helper that decides which parameters the effects chain reads. Declare it in the header as `static bool isEffectParameter(const std::string&) noexcept;`

```cpp
bool SpaceDustAudioProcessor::isEffectParameter (const std::string& id) noexcept
{
    // The chain from runEffectsChain, by the prefix each stage's parameters use.
    // A knob outside this list is a voice knob, which the per-sample voice loop
    // already reaches without any chunking.
    // "master" is deliberately NOT here. masterVolume is one of the six
    // destinations the VOICE already applies per sample; listing it would chunk
    // the whole effects chain for a knob that does not need it, and risks the
    // same knob being applied twice.
    static const char* const prefixes[] = {
        "reverb", "delay", "grain", "phaser", "flanger", "transient",
        "compressor", "softClipper", "lofi", "bitCrusher", "tranceGate",
        "finalEQ"
    };

    for (const char* p : prefixes)
        if (id.rfind (p, 0) == 0)
            return true;

    return false;
}
```

- [ ] **Step 3: Call the rebuild wherever routings change**

At the end of the constructor, after `modDestinations.build (apvts);`:

```cpp
    rebuildCompiledRoutings();
```

And at the end of `setStateInformation`, after the matrix is restored:

```cpp
    rebuildCompiledRoutings();
```

- [ ] **Step 4: Fill `lfoValues` and refresh the modulated values per chunk**

The LFO buffers already exist (`lfo1Buffer`, `lfo2Buffer`, plus `lfo3Buffer` and `lfo4Buffer` from Task 9). In `runEffectsChain`, at the top:

```cpp
    // The LFO value at the FIRST sample of this chunk stands for the whole
    // chunk. At 32 samples that is a control rate near 1400 Hz, which is what
    // makes an assigned effect knob move smoothly rather than in steps.
    for (int i = 0; i < spacedust::numLfos; ++i)
        lfoValues[i] = lfoBufferFor (i).getSample (0, startSampleInBlock);

    const int liveIndex = liveCompiled.load (std::memory_order_acquire);
    const auto& compiled = compiledBuffers[liveIndex];

    for (int i = 0; i < modDestinations.size(); ++i)
        destBases[(size_t) i] = modDestinations.paramAt (i)->convertFrom0to1 (
                                    modDestinations.paramAt (i)->getValue());

    spacedust::ModMatrix::applyCompiled (compiled.data(), (int) compiled.size(),
                                         destBases.data(), destRanges.data(),
                                         modDestinations.size(),
                                         lfoValues, destModulated.data());
```

Then every effect parameter read inside `runEffectsChain` uses `modulatedValue(slot)` in place of `safeGetParam`. Add:

```cpp
    float modulatedValue (int slot) const noexcept
    {
        return destModulated[(size_t) slot];
    }
```

Cache each effect parameter's slot once in `prepareToPlay` rather than calling `slotFor` per chunk.

- [ ] **Step 5: Leave the six voice destinations exactly as they are**

Do **not** change `SynthVoice.cpp:1712-1747`. Pitch, filter cutoff, master volume, Osc 1, Osc 2 and noise volume keep their existing per-sample code. Extend that block only to read LFO 3 and 4 buffers as well, in Task 9.

For voice knobs that are NOT one of those six, add to the voice's per-sample loop:

```cpp
            // Voice knobs reached by the matrix. The six that predate the matrix
            // keep their own paths above; this covers everything else the voice
            // reads, so an assigned shaping or unison knob moves per sample too.
            for (int i = 0; i < numVoiceModSlots; ++i)
                voiceModValues[i] = processor->modulatedValue (voiceModSlots[i]);
```

- [ ] **Step 6: Verify the chunk audit still passes**

```bash
cmake --build build --config Release --target SpaceDustChunkAudit
./build/SpaceDustChunkAudit_artefacts/Release/SpaceDustChunkAudit.exe
```

Expected: still `samples differing: 0`. With no routings assigned, nothing may change.

- [ ] **Step 7: Prove an assigned knob actually moves the sound**

Step 6 proves nothing changed when nothing is assigned. This proves something
changes when something is. Add to `tools/chunkaudit/chunk_audit_main.cpp`, before
`return 0`:

```cpp
    // -- the destination sweep --
    // A routing that reaches an effect must audibly change the output, and it
    // must do so through the chunked path. Without this, a matrix that compiles
    // to nothing would pass every test above.
    {
        std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
        auto* sd = dynamic_cast<SpaceDustAudioProcessor*> (proc.get());

        // Reverb on and wide open, so the knob under test has something to move.
        auto set = [&] (const char* id, float value)
        {
            if (auto* p = sd->getValueTreeState().getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (value));
        };

        set ("reverbEnabled", 1.0f);
        set ("reverbWetMix", 0.5f);
        set ("lfo1Enabled", 1.0f);
        set ("lfo1Depth", 100.0f);
        set ("lfo1Sync", 0.0f);
        set ("lfo1Rate", 6.0f);

        const auto flat = renderWith (*sd);

        sd->modMatrix.setRouting (0, "reverbWetMix", 1.0f);
        sd->rebuildCompiledRoutings();

        const auto moved = renderWith (*sd);

        double biggest = 0.0;
        for (size_t i = 0; i < flat.size() && i < moved.size(); ++i)
            biggest = juce::jmax (biggest, std::abs ((double) flat[i] - (double) moved[i]));

        std::printf ("\n  destination sweep, LFO 1 -> reverbWetMix\n");
        std::printf ("  largest difference: %.9f\n", biggest);

        if (biggest < 1.0e-6)
        {
            // Never the string "error :" -- MSBuild reads that as a build failure.
            std::printf ("  FAIL  an assigned routing changed nothing.\n");
            return 1;
        }

        std::printf ("  an assigned routing moves the sound.\n");
    }
```

Refactor the existing `render` into `renderWith(SpaceDustAudioProcessor&)` plus a
thin wrapper, so both checks drive the same code.

- [ ] **Step 8: Run it**

```bash
cmake --build build --config Release --target SpaceDustChunkAudit
./build/SpaceDustChunkAudit_artefacts/Release/SpaceDustChunkAudit.exe
```

Expected: `samples differing: 0` for the equality check, and a largest difference
well above `1.0e-6` for the sweep. Both must pass.

- [ ] **Step 9: Commit**

```bash
git add Source/PluginProcessor.h Source/PluginProcessor.cpp Source/SynthVoice.h Source/SynthVoice.cpp tools/chunkaudit/chunk_audit_main.cpp
git commit -m "feat(mod): routings reach the audio, one sample at a time"
```

---

## Task 5: The modulatable knob

**Files:**
- Create: `Source/ModulatableKnob.h`, `Source/ModulatableKnob.cpp`, `Source/AssignModeState.h`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `spacedust::ModMatrix`, `spacedust::numLfos`.
- Produces:
  - `spacedust::AssignModeState` with `activeLfo() const -> int` (-1 when off), `setActiveLfo(int)`, `addListener`/`removeListener`, `colourFor(int lfo) -> juce::Colour`
  - `ModulatableKnob : public juce::Component` wrapping a `juce::Slider&`, with `setDestination(const std::string&)`

`PluginEditor.cpp` is already 8588 lines. One wrapper, not 150 edited call sites.

- [ ] **Step 1: Write `Source/AssignModeState.h`**

```cpp
#pragma once

#include "ModMatrix.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spacedust
{
    /** Which LFO is being assigned, and who is told when that changes.

        One of these lives in the editor. Every ModulatableKnob listens to it, so
        entering assign mode lights up every knob at once without the editor
        holding a list of them. */
    class AssignModeState : public juce::ChangeBroadcaster
    {
    public:
        /** -1 when assign mode is off. */
        int activeLfo() const noexcept { return active; }

        void setActiveLfo (int lfo)
        {
            const int clamped = (lfo >= 0 && lfo < numLfos) ? lfo : -1;

            if (clamped == active)
                return;

            active = clamped;
            sendChangeMessage();
        }

        /** Each LFO owns a colour, so which LFO holds a knob is readable at a
            glance once there are four of them. LFO 1 is the blue Giuseppe asked
            for; the other three are picked to stay apart on a dark background. */
        static juce::Colour colourFor (int lfo)
        {
            switch (lfo)
            {
                case 0:  return juce::Colour (0xff4aa3ff);  // blue
                case 1:  return juce::Colour (0xff4ad991);  // green
                case 2:  return juce::Colour (0xffffb347);  // amber
                case 3:  return juce::Colour (0xffe07aff);  // magenta
                default: return juce::Colours::grey;
            }
        }

    private:
        int active = -1;
    };
}
```

- [ ] **Step 2: Write `Source/ModulatableKnob.h`**

```cpp
#pragma once

#include "AssignModeState.h"
#include "ModMatrix.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>

/** One knob's modulation behaviour, laid over the top of an existing Slider.

    Sits in front of the slider it wraps and is transparent to the mouse while
    assign mode is off, so the slider behaves exactly as it always did. While
    assign mode is on it takes the mouse instead: a drag sets the ROUTING
    amount, not the knob's own value.

    It also draws the indicator bar, which is visible whether or not assign mode
    is on, because what moves in a patch should always be readable. */
class ModulatableKnob : public juce::Component,
                        private juce::ChangeListener
{
public:
    ModulatableKnob (juce::Slider& knobToWrap,
                     spacedust::ModMatrix& matrixToEdit,
                     spacedust::AssignModeState& modeState,
                     std::function<void()> onRoutingChanged);

    ~ModulatableKnob() override;

    /** The APVTS parameter id this knob drives. */
    void setDestination (std::string parameterId) { destination = std::move (parameterId); }

    const std::string& getDestination() const noexcept { return destination; }

    /** Where the LFOs are in their cycles, 0..1, for the bar's marker.
        Pushed in by the editor's repaint timer rather than pulled, so one timer
        serves every knob. */
    void setLfoPhases (const float* phases01);

    void paint (juce::Graphics&) override;
    void resized() override;

    bool hitTest (int x, int y) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    /** The strip to the right of the knob that holds the indicator bar. */
    static constexpr int barWidth = 6;
    static constexpr int barGap   = 3;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::Rectangle<int> barArea() const;

    /** Which LFO owns the bar lane under this point, or -1. */
    int lfoLaneAt (juce::Point<int> position) const;

    juce::Slider&               knob;
    spacedust::ModMatrix&       matrix;
    spacedust::AssignModeState& mode;
    std::function<void()>       routingChanged;

    std::string destination;

    float phases[spacedust::numLfos] { 0.0f, 0.0f, 0.0f, 0.0f };

    /** Set while a drag is running, so the percentage reads out only then. */
    bool  dragging = false;
    int   dragLfo = -1;
    float dragStartAmount = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModulatableKnob)
};
```

- [ ] **Step 3: Write `Source/ModulatableKnob.cpp`**

```cpp
#include "ModulatableKnob.h"

namespace
{
    /** A full-height drag sets the amount from one end to the other. 150 pixels
        is roughly the height of a knob plus its label, which makes a normal
        gesture cover the range without feeling twitchy. */
    constexpr float dragPixelsForFullRange = 150.0f;
}

ModulatableKnob::ModulatableKnob (juce::Slider& knobToWrap,
                                  spacedust::ModMatrix& matrixToEdit,
                                  spacedust::AssignModeState& modeState,
                                  std::function<void()> onRoutingChanged)
    : knob (knobToWrap),
      matrix (matrixToEdit),
      mode (modeState),
      routingChanged (std::move (onRoutingChanged))
{
    setInterceptsMouseClicks (false, false);
    mode.addChangeListener (this);
}

ModulatableKnob::~ModulatableKnob()
{
    mode.removeChangeListener (this);
}

void ModulatableKnob::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // While assign mode is on this layer takes the mouse; while it is off the
    // slider underneath gets it and behaves exactly as it always did.
    const bool assigning = mode.activeLfo() >= 0;
    setInterceptsMouseClicks (assigning, assigning);
    repaint();
}

juce::Rectangle<int> ModulatableKnob::barArea() const
{
    return getLocalBounds().removeFromRight (barWidth);
}

bool ModulatableKnob::hitTest (int x, int y)
{
    // The bar is clickable even outside assign mode, so an amount can be edited
    // without entering the mode at all. Everything else falls through.
    if (barArea().contains (x, y) && matrix.hasAnyRouting (destination))
        return true;

    return mode.activeLfo() >= 0;
}

int ModulatableKnob::lfoLaneAt (juce::Point<int> position) const
{
    auto bar = barArea();

    if (! bar.contains (position))
        return -1;

    // Where two LFOs reach one knob the bar is split into one lane each, in LFO
    // order, so which lane belongs to which LFO never moves as amounts change.
    std::vector<int> lanes;
    for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
        if (matrix.amountFor (lfo, destination) != 0.0f)
            lanes.push_back (lfo);

    if (lanes.empty())
        return -1;

    const int laneWidth = juce::jmax (1, bar.getWidth() / (int) lanes.size());
    const int index = juce::jlimit (0, (int) lanes.size() - 1,
                                    (position.x - bar.getX()) / laneWidth);

    return lanes[(size_t) index];
}

void ModulatableKnob::mouseDown (const juce::MouseEvent& event)
{
    dragLfo = lfoLaneAt (event.getPosition());

    // A press away from the bar in assign mode assigns the LFO being assigned.
    if (dragLfo < 0)
        dragLfo = mode.activeLfo();

    if (dragLfo < 0)
        return;

    dragging = true;
    dragStartAmount = matrix.amountFor (dragLfo, destination);
}

void ModulatableKnob::mouseDrag (const juce::MouseEvent& event)
{
    if (! dragging || dragLfo < 0)
        return;

    // Up is positive. getDistanceFromDragStartY grows downwards, so it is negated.
    const float delta = -(float) event.getDistanceFromDragStartY() / dragPixelsForFullRange;
    const float amount = juce::jlimit (-1.0f, 1.0f, dragStartAmount + delta);

    // An amount of zero removes the routing, so dragging back through the middle
    // undoes an assignment rather than leaving a dead entry behind.
    matrix.setRouting (dragLfo, destination, amount);

    if (routingChanged)
        routingChanged();

    repaint();
}

void ModulatableKnob::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    dragLfo = -1;
    repaint();
}

void ModulatableKnob::setLfoPhases (const float* phases01)
{
    bool changed = false;

    for (int i = 0; i < spacedust::numLfos; ++i)
    {
        if (phases[i] != phases01[i])
        {
            phases[i] = phases01[i];
            changed = true;
        }
    }

    // Only repaint when a bar is actually drawn here. The timer runs at 30 Hz
    // and there are about 150 of these.
    if (changed && matrix.hasAnyRouting (destination))
        repaint (barArea());
}

void ModulatableKnob::resized()
{
}

void ModulatableKnob::paint (juce::Graphics& g)
{
    const int assigning = mode.activeLfo();

    // -- the ring, while assign mode is on --
    if (assigning >= 0)
    {
        auto ring = getLocalBounds().withTrimmedRight (barWidth + barGap).toFloat().reduced (1.0f);
        g.setColour (spacedust::AssignModeState::colourFor (assigning).withAlpha (0.85f));
        g.drawRoundedRectangle (ring, 4.0f, 2.0f);
    }

    // -- the bar, whether or not assign mode is on --
    std::vector<int> lanes;
    for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
        if (matrix.amountFor (lfo, destination) != 0.0f)
            lanes.push_back (lfo);

    if (! lanes.empty())
    {
        auto bar = barArea().toFloat();
        const float laneWidth = bar.getWidth() / (float) lanes.size();

        for (size_t i = 0; i < lanes.size(); ++i)
        {
            const int lfo = lanes[i];
            const float amount = matrix.amountFor (lfo, destination);
            const auto colour = spacedust::AssignModeState::colourFor (lfo);

            auto lane = bar.withWidth (laneWidth).withX (bar.getX() + laneWidth * (float) i);

            g.setColour (colour.withAlpha (0.20f));
            g.fillRect (lane);

            // The filled part is the amount, measured from the middle, so a
            // negative amount reads as clearly as a positive one.
            const float mid = lane.getCentreY();
            const float reach = lane.getHeight() * 0.5f * std::abs (amount);

            g.setColour (colour.withAlpha (0.55f));
            g.fillRect (lane.withY (amount >= 0.0f ? mid - reach : mid)
                            .withHeight (reach));

            // Where the LFO is in its cycle right now.
            const float y = lane.getBottom() - lane.getHeight() * phases[lfo];
            g.setColour (colour);
            g.fillRect (lane.getX(), y - 1.0f, lane.getWidth(), 2.0f);
        }
    }

    // -- the percentage, only while a drag is running --
    if (dragging && dragLfo >= 0)
    {
        const int percent = juce::roundToInt (matrix.amountFor (dragLfo, destination) * 100.0f);
        const auto text = juce::String (percent) + "%";

        auto label = getLocalBounds().removeFromTop (14).removeFromRight (46).translated (-barWidth, 0);

        g.setColour (juce::Colours::black.withAlpha (0.70f));
        g.fillRoundedRectangle (label.toFloat(), 3.0f);
        g.setColour (spacedust::AssignModeState::colourFor (dragLfo));
        g.setFont (11.0f);
        g.drawText (text, label, juce::Justification::centred);
    }
}
```

- [ ] **Step 4: Add the files to the plugin build**

In `CMakeLists.txt`, add to the `SpaceDust` target's `target_sources`:

```cmake
    Source/ModulatableKnob.cpp
```

- [ ] **Step 5: Build**

```bash
cmake --build build --config Release --target SpaceDust
```

Expected: builds clean. Nothing uses the class yet.

- [ ] **Step 6: Commit**

```bash
git add Source/AssignModeState.h Source/ModulatableKnob.h Source/ModulatableKnob.cpp CMakeLists.txt
git commit -m "feat(mod): a knob that can be pointed at, and a bar that shows what moves"
```

---

## Task 6: Assign mode in the editor

**Files:**
- Modify: `Source/PluginEditor.h`, `Source/PluginEditor.cpp`

**Interfaces:**
- Consumes: `spacedust::AssignModeState`, `ModulatableKnob`, `DestinationTable::isLegalDestination`.
- Produces: `SpaceDustAudioProcessorEditor::assignMode`, `exitLfoModeButton`, `wrapKnob(juce::Slider&, const juce::String& paramId)`.

- [ ] **Step 1: Add the state, the button, and the wrapper list**

In `Source/PluginEditor.h`:

```cpp
#include "AssignModeState.h"
#include "ModulatableKnob.h"
```

```cpp
    spacedust::AssignModeState assignMode;

    /** One per assignable knob, created by wrapKnob and owned here. */
    juce::OwnedArray<ModulatableKnob> modKnobs;

    /** Appears in the tab strip only while assign mode is on. */
    juce::TextButton exitLfoModeButton { "Exit LFO Mode" };

    /** Lay a ModulatableKnob over one slider and remember it.

        Call once per assignable knob, from the editor constructor, after the
        slider has been added. The wrapper is positioned in resized() to match
        the slider's bounds plus the bar strip on its right. */
    void wrapKnob (juce::Slider& slider, const juce::String& parameterId);

    /** Push the LFO phases into every wrapper. Driven by a 30 Hz timer. */
    void refreshModIndicators();
```

- [ ] **Step 2: Implement `wrapKnob`**

In `Source/PluginEditor.cpp`:

```cpp
void SpaceDustAudioProcessorEditor::wrapKnob (juce::Slider& slider,
                                              const juce::String& parameterId)
{
    auto* param = audioProcessor.getValueTreeState().getParameter (parameterId);
    auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param);

    // Silently skipping an illegal destination is what lets the call list below
    // be written once per knob without each caller checking first.
    if (ranged == nullptr
        || ! spacedust::DestinationTable::isLegalDestination (*ranged))
        return;

    auto* wrapper = modKnobs.add (new ModulatableKnob (
        slider,
        audioProcessor.modMatrix,
        assignMode,
        [this] { audioProcessor.rebuildCompiledRoutings(); }));

    wrapper->setDestination (parameterId.toStdString());

    // Added to the same parent as the slider, directly above it, so it can take
    // the mouse in assign mode and draw its bar beside the knob.
    if (auto* parent = slider.getParentComponent())
    {
        parent->addAndMakeVisible (wrapper);
        wrapper->toFront (false);
    }
}
```

- [ ] **Step 3: Keep each wrapper over its knob**

In the editor's `resized()`, after the existing layout has run:

```cpp
    // After everything else, because each wrapper takes its bounds from the
    // slider it wraps. The strip on the right is where the indicator bar goes.
    for (auto* wrapper : modKnobs)
        wrapper->setBounds (wrapper->getWrappedBounds()
                                .withWidth (wrapper->getWrappedBounds().getWidth()
                                            + ModulatableKnob::barWidth
                                            + ModulatableKnob::barGap));
```

Add to `ModulatableKnob`:

```cpp
    juce::Rectangle<int> getWrappedBounds() const { return knob.getBounds(); }
```

- [ ] **Step 4: Wrap every assignable knob**

In the editor constructor, after all sliders and attachments are set up, add one `wrapKnob` call per knob. For the arrays:

```cpp
    // -- assignable knobs --
    // wrapKnob refuses anything that is not a legal destination, so this list
    // may name a knob without checking what kind of parameter it drives.
    wrapKnob (filterCutoffSlider,    "filterCutoff");
    wrapKnob (filterResonanceSlider, "filterResonance");
    wrapKnob (osc1LevelSlider,       "osc1Level");
    wrapKnob (osc2LevelSlider,       "osc2Level");
    wrapKnob (osc1PanSlider,         "osc1Pan");
    wrapKnob (osc2PanSlider,         "osc2Pan");
    wrapKnob (noiseLevelSlider,      "noiseLevel");
    wrapKnob (osc1DetuneSlider,      "osc1Detune");
    wrapKnob (osc2DetuneSlider,      "osc2Detune");
    wrapKnob (masterVolumeSlider,    "masterVolume");
    // ... one line per remaining assignable knob.

    // Verified against createParameterLayout: osc1BendPlus, osc1BendMinus,
    // osc1BendPlusMinus, osc1Spectrum, osc1Sync. wrapKnob skips an unknown id
    // SILENTLY, so a wrong one here ships as a knob that never lights up.
    static const char* const shapingIds[numShapingKnobs] =
        { "BendPlus", "BendMinus", "BendPlusMinus", "Spectrum", "Sync" };

    for (int i = 0; i < numShapingKnobs; ++i)
    {
        wrapKnob (osc1ShapingSliders[i], juce::String ("osc1") + shapingIds[i]);
        wrapKnob (osc2ShapingSliders[i], juce::String ("osc2") + shapingIds[i]);
        wrapKnob (subOscShapingSliders[i], juce::String ("subOsc") + shapingIds[i]);
    }

    static const char* const unisonIds[numUnisonKnobs] =
        { "UnisonVoices", "UnisonDetune", "UnisonWidth", "UnisonPhase" };

    for (int i = 0; i < numUnisonKnobs; ++i)
    {
        wrapKnob (osc1UnisonSliders[i],   juce::String ("osc1")   + unisonIds[i]);
        wrapKnob (osc2UnisonSliders[i],   juce::String ("osc2")   + unisonIds[i]);
        wrapKnob (subOscUnisonSliders[i], juce::String ("subOsc") + unisonIds[i]);
        wrapKnob (noiseUnisonSliders[i],  juce::String ("noise")  + unisonIds[i]);
    }
```

Confirm each id against `createParameterLayout` before writing it. A wrong id is skipped silently, so the knob simply will not light up.

- [ ] **Step 5: Add the Assign buttons and Exit LFO Mode**

In the editor constructor:

```cpp
    // One Assign button per LFO panel. Pressing it puts the editor into assign
    // mode for that LFO; the knobs light up in that LFO's colour.
    for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
    {
        auto* button = lfoAssignButtons.add (new juce::TextButton ("Assign"));
        button->setColour (juce::TextButton::buttonColourId,
                           spacedust::AssignModeState::colourFor (lfo).withAlpha (0.35f));
        button->onClick = [this, lfo] { assignMode.setActiveLfo (lfo); };
        addAndMakeVisible (button);
    }

    // Lives in the tab strip, and is only there while the mode is on.
    exitLfoModeButton.setColour (juce::TextButton::buttonColourId,
                                 juce::Colour (0xff4aa3ff));
    exitLfoModeButton.onClick = [this] { assignMode.setActiveLfo (-1); };
    addChildComponent (exitLfoModeButton);

    assignMode.addChangeListener (this);
```

In `changeListenerCallback`, for `&assignMode`:

```cpp
        const bool assigning = assignMode.activeLfo() >= 0;

        // Hidden on the Modulation page, because no LFO control is a legal
        // destination there. The MODE stays on, so switching back to another tab
        // carries on where it left off.
        const bool onModulationPage = tabbedComponent.getCurrentTabIndex() == 1;

        exitLfoModeButton.setVisible (assigning);

        for (auto* wrapper : modKnobs)
            wrapper->setVisible (! (assigning && onModulationPage));

        resized();
```

- [ ] **Step 6: Escape leaves the mode**

```cpp
bool SpaceDustAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && assignMode.activeLfo() >= 0)
    {
        assignMode.setActiveLfo (-1);
        return true;
    }

    return false;
}
```

- [ ] **Step 7: Place Exit LFO Mode in the tab strip**

In `resized()`, where the tabbed component is laid out (around line 8357):

```cpp
    // In the tab strip, at its right-hand end, so leaving the mode is where the
    // eye already is when switching tabs.
    if (exitLfoModeButton.isVisible())
    {
        auto strip = tabbedComponent.getTabbedButtonBar().getBounds();
        exitLfoModeButton.setBounds (strip.removeFromRight (120).reduced (4, 4));
        exitLfoModeButton.toFront (false);
    }
```

- [ ] **Step 8: Build, deploy, and look at it**

```bash
cmake --build build --config Release --target SpaceDust
cmake --build build --config Release --target SpaceDust_VST3
```

Launch the Standalone through `explorer.exe`, never from an elevated shell — it inherits admin and drag-and-drop dies silently. Press Assign on LFO 1 and confirm: the knobs on Main gain a blue ring, Exit LFO Mode appears in the tab strip, a drag on a knob shows a percentage and does not move the knob, and Escape leaves the mode.

- [ ] **Step 9: Commit**

```bash
git add Source/PluginEditor.h Source/PluginEditor.cpp
git commit -m "feat(mod): point at a knob to assign an LFO to it"
```

---

## Task 7: The live indicator

**Files:**
- Modify: `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp:1700-1760`
- Modify: `Source/PluginEditor.h`, `Source/PluginEditor.cpp`

**Interfaces:**
- Consumes: `ModulatableKnob::setLfoPhases` (Task 5).
- Produces: `SpaceDustAudioProcessor::lfoPhase01[numLfos]` as `std::atomic<float>`.

- [ ] **Step 1: Publish each LFO's phase**

In `Source/PluginProcessor.h`:

```cpp
    /** Where each LFO is in its cycle, 0..1, for the editor's indicator bars.

        Written once per block by the audio thread and read by a 30 Hz timer on
        the message thread. Relaxed atomics: a bar that is one frame old is
        invisible, and a lock here would be worse than the staleness. */
    std::atomic<float> lfoPhase01[spacedust::numLfos];
```

In `processBlock`, after the LFO buffers are filled:

```cpp
    // The phase at the END of the block is the one closest to what the player
    // is hearing by the time the editor next paints.
    lfoPhase01[0].store (lfo1PhaseNow, std::memory_order_relaxed);
    lfoPhase01[1].store (lfo2PhaseNow, std::memory_order_relaxed);
    lfoPhase01[2].store (lfo3PhaseNow, std::memory_order_relaxed);
    lfoPhase01[3].store (lfo4PhaseNow, std::memory_order_relaxed);
```

- [ ] **Step 2: Drive the bars from a timer**

In the editor, add a timer id and start it only while a bar is on screen:

```cpp
void SpaceDustAudioProcessorEditor::refreshModIndicators()
{
    float phases[spacedust::numLfos];

    for (int i = 0; i < spacedust::numLfos; ++i)
        phases[i] = audioProcessor.lfoPhase01[i].load (std::memory_order_relaxed);

    for (auto* wrapper : modKnobs)
        wrapper->setLfoPhases (phases);
}
```

Call it from the editor's existing MultiTimer at a new id, at 33 ms. `setLfoPhases` repaints only knobs that actually have a routing, so an unassigned patch costs one loop over the list and no painting.

- [ ] **Step 3: Build, deploy, and watch it**

Assign LFO 1 to Filter Cutoff, turn the LFO on, hold a note, and confirm the marker moves in the bar beside the cutoff knob and stops when the LFO is switched off.

- [ ] **Step 4: Commit**

```bash
git add Source/PluginProcessor.h Source/PluginProcessor.cpp Source/PluginEditor.h Source/PluginEditor.cpp
git commit -m "feat(mod): the bar beside a knob shows where its LFO is now"
```

---

## Task 8: Delete the mod filters

**Files:**
- Modify: `Source/PluginProcessor.cpp:4102-4195`, `Source/SynthVoice.h`, `Source/SynthVoice.cpp`, `Source/PluginEditor.h`, `Source/PluginEditor.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing. This task only removes.

- [ ] **Step 1: Remove the 16 parameters**

Delete the parameter definitions at `PluginProcessor.cpp:4102-4195`: `modFilter1Show`, `modFilter2Show`, `modFilter1LinkToMaster`, `modFilter2LinkToMaster`, `modFilter1Mode`, `modFilter1Cutoff`, `modFilter1Resonance`, `modFilter1KeyTrack`, `modFilter1NoteLock`, `modFilter1HarmonicLock`, and the six matching `modFilter2*`.

- [ ] **Step 2: Remove the per-voice DSP**

Remove all 84 `modFilter` references in `Source/SynthVoice.cpp` and the 16 in `Source/SynthVoice.h`: the two extra filter instances, their prepare and reset calls, their per-sample processing, and their setters.

- [ ] **Step 3: Remove the UI**

Remove all 230 `modFilter` references in `Source/PluginEditor.cpp` and the 41 in `Source/PluginEditor.h`: the component declarations, the `addAndMakeVisible` calls in `ModulationPageComponent`, the layout in `resized()`, the attachments, and the entries in `relayoutTriggerParams()`.

- [ ] **Step 4: Remove the Destination drop-downs**

Delete `lfo1Target` and `lfo2Target` from `createParameterLayout`, `lfo1TargetCombo` / `lfo2TargetCombo` and their labels and attachments from the editor, and `setLfoTargets` plus `lfo1TargetCached` / `lfo2TargetCached` from `SynthVoice`.

Replace the target branches at `SynthVoice.cpp:1712-1747` with reads from the matrix, keeping the same six code paths so the sound of a migrated patch is unchanged.

- [ ] **Step 5: Verify nothing is left**

```bash
grep -rn "modFilter\|lfo1Target\|lfo2Target" Source/
```

Expected: no output.

- [ ] **Step 6: Build and run every test**

```bash
cmake --build build --config Release --target SpaceDust
cmake --build build --config Release --target modmatrix-test
cmake --build build --config Release --target SpaceDustChunkAudit
```

- [ ] **Step 7: Commit**

```bash
git add Source/
git commit -m "fix(mod): the mod filters and the destination menus have no job left"
```

---

## Task 9: LFO 3 and LFO 4

**Files:**
- Modify: `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`, `Source/PluginEditor.h`, `Source/PluginEditor.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: parameters `lfo3*` and `lfo4*`, and `SpaceDustAudioProcessor::lfoBufferFor(int) -> juce::AudioBuffer<float>&`.

- [ ] **Step 1: Add the parameters at the END of the layout**

Nine per LFO, matching LFO 1 and 2 exactly, minus the deleted Target. Appended at the end of `createParameterLayout` because the VST3 parameter index is an automation contract:

```
lfo3Waveform, lfo3Enabled, lfo3Depth, lfo3Sync, lfo3Rate, lfo3Phase,
lfo3TripletEnabled, lfo3TripletStraightToggle, lfo3Retrigger
lfo4Waveform, lfo4Enabled, lfo4Depth, lfo4Sync, lfo4Rate, lfo4Phase,
lfo4TripletEnabled, lfo4TripletStraightToggle, lfo4Retrigger
```

Copy the ranges and defaults from the `lfo1*` definitions at `PluginProcessor.cpp:3964-4029`.

- [ ] **Step 2: Confirm the buffers already carry them**

`lfoBuffers`, `lfoBufferFor` and the single LFO fill loop were brought forward into
Task 4 step 0, because Tasks 4 and 7 both read four buffers. Nothing to refactor here.

Confirm only that buffers 2 and 3, silent until this task, now fill from the new
`lfo3*` and `lfo4*` parameters: hold a note with LFO 3 enabled and check its buffer is
no longer all zeros.

- [ ] **Step 3: One panel layout for four LFOs**

Replace the two near-identical LFO panel blocks in `ModulationPageComponent` with one function taking an LFO index, and call it four times. The four panels take the space Mod Filter 1 and 2 left behind.

- [ ] **Step 4: Build, deploy, and check**

Confirm all four LFO panels appear on the Modulation page, each has its own Assign button in its own colour, and assigning LFO 3 to a knob moves it.

- [ ] **Step 5: Commit**

```bash
git add Source/
git commit -m "feat(mod): four LFOs, in the room the mod filters left"
```

---

## Task 10: Migration

**Files:**
- Modify: `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp:3152-3234`

**Interfaces:**
- Consumes: `spacedust::ModMatrix::setRouting`.
- Produces: `SpaceDustAudioProcessor::migrateLfoTargetsIfOld(juce::ValueTree&, int stateVersion, spacedust::ModMatrix&)`.

- [ ] **Step 1: Raise the state version**

Increment `currentStateVersion` by one in `Source/PluginProcessor.h`.

- [ ] **Step 2: Write the migration**

```cpp
void SpaceDustAudioProcessor::migrateLfoTargetsIfOld (juce::ValueTree& state,
                                                      int stateVersion,
                                                      spacedust::ModMatrix& matrix)
{
    // Patches saved at the new version already carry a MODMATRIX.
    if (stateVersion >= currentStateVersion)
        return;

    // 0=Pitch, 1=Filter, 2=MasterVol, 3=Osc1, 4=Osc2, 5=Noise -- the order the
    // deleted lfo1Target and lfo2Target choice used.
    static const char* const oldTargets[] = {
        "osc1CoarseTune", "filterCutoff", "masterVolume",
        "osc1Level", "osc2Level", "noiseLevel"
    };

    for (int lfo = 0; lfo < 2; ++lfo)
    {
        const auto id = juce::String ("lfo") + juce::String (lfo + 1) + "Target";
        auto node = state.getChildWithProperty ("id", id);

        if (! node.isValid())
            continue;

        const int target = (int) node.getProperty ("value", -1);

        if (target < 0 || target >= (int) std::size (oldTargets))
            continue;

        // +1.0, NOT that LFO's Depth. Depth survives this migration untouched
        // and already scales the LFO output; taking the amount from it as well
        // would square the depth and halve the movement of every saved patch.
        matrix.setRouting (lfo, oldTargets[target], 1.0f);
    }
}
```

- [ ] **Step 3: Call it**

In `setStateInformation`, beside `migrateLfoRatesIfOld`:

```cpp
            // Before replaceState, because it reads the OLD lfo*Target values,
            // which this build no longer has parameters for.
            migrateLfoTargetsIfOld (restored, savedVersion, modMatrix);
```

- [ ] **Step 4: Test it by hand**

Check out the commit before Task 8, build, save a patch with LFO 1 set to Filter and Depth at 50. Check out the current build, load that patch, and confirm the filter cutoff knob shows a blue indicator bar and the filter still wobbles at the same depth it did.

- [ ] **Step 5: Commit**

```bash
git add Source/PluginProcessor.h Source/PluginProcessor.cpp
git commit -m "fix(mod): a patch saved with a destination menu keeps its movement"
```

---

## Task 11: The pitch curve, and its sound

**Files:**
- Create: `Source/PitchCurve.h`, `Source/PitchCurve.cpp`
- Modify: `tools/modmatrixtest/mod_matrix_test.cpp`, `CMakeLists.txt`
- Modify: `Source/PluginProcessor.cpp`, `Source/SynthVoice.h`, `Source/SynthVoice.cpp:1645-1667`

**Interfaces:**
- Consumes: nothing.
- Produces: `spacedust::PitchCurve` with `addPoint(float t01, float semitones)`, `pointCount() const -> int`, `pointAt(int) const -> Point`, `valueAt(float t01) const -> float`, `clear()`, `isFlat() const -> bool`; and `spacedust::pitchCurveToXml` / `pitchCurveFromXml` (tag `PITCHCURVE`).

- [ ] **Step 1: Write the failing test**

Append to `tools/modmatrixtest/mod_matrix_test.cpp`, before the `failures` report:

```cpp
    // -- the pitch curve --
    {
        PitchCurve c;

        // Flat and silent by default, which is what keeps a patch with no drawn
        // curve sounding exactly as it did.
        check(c.isFlat(), "a new curve is flat");
        checkNear(c.valueAt(0.0f), 0.0, 1e-6, "a flat curve bends nothing at the start");
        checkNear(c.valueAt(1.0f), 0.0, 1e-6, "a flat curve bends nothing at the end");

        // A fall from +12 semitones to 0, which is what the old ramp did.
        c.clear();
        c.addPoint(0.0f, 12.0f);
        c.addPoint(1.0f, 0.0f);

        check(!c.isFlat(), "a drawn curve is not flat");
        checkNear(c.valueAt(0.0f),  12.0, 1e-5, "the curve starts where it was drawn");
        checkNear(c.valueAt(0.5f),   6.0, 1e-5, "and runs straight between points");
        checkNear(c.valueAt(1.0f),   0.0, 1e-5, "and ends where it was drawn");

        // Past the end the last value is HELD, which is what makes a curve drawn
        // back to zero behave exactly as the old ramp did.
        checkNear(c.valueAt(2.0f), 0.0, 1e-5, "past the end the last value holds");

        // Before the first point the first value holds.
        c.clear();
        c.addPoint(0.5f, 6.0f);
        checkNear(c.valueAt(0.0f), 6.0, 1e-5, "before the first point the first value holds");

        // Points added out of order are sorted, so drawing right to left works.
        c.clear();
        c.addPoint(1.0f, 0.0f);
        c.addPoint(0.0f, 12.0f);
        checkNear(c.valueAt(0.25f), 9.0, 1e-5, "points are sorted by time");
    }
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build --config Release --target modmatrix-test
```

Expected: FAILS to compile — `PitchCurve` does not exist.

- [ ] **Step 3: Write `Source/PitchCurve.h`**

```cpp
#pragma once

#include <vector>

namespace spacedust
{
    /** A pitch shape drawn by hand, in semitones over time.

        Replaces the three Pitch Env knobs, which could only make one straight
        fall. Free of JUCE for the same reason ModMatrix is.

        Time is 0..1 across whatever the Time knob is set to. Value is
        semitones, and the editor limits it to -24..+24.

        Outside the drawn range the nearest end value is HELD. That is what makes
        a curve drawn from some pitch back to zero behave exactly as the old ramp
        did, so a migrated patch sounds the same. */
    class PitchCurve
    {
    public:
        struct Point
        {
            float t01 = 0.0f;
            float semitones = 0.0f;
        };

        void clear();

        /** Points are kept sorted by time, so drawing right to left works. */
        void addPoint (float t01, float semitones);

        int pointCount() const noexcept { return (int) points.size(); }

        Point pointAt (int index) const { return points[(size_t) index]; }

        /** Straight between points, held past either end. */
        float valueAt (float t01) const noexcept;

        /** No points, or every point at zero. A flat curve is skipped entirely
            in the voice, so a patch that draws nothing costs nothing. */
        bool isFlat() const noexcept;

    private:
        std::vector<Point> points;
    };
}
```

- [ ] **Step 4: Write `Source/PitchCurve.cpp`**

```cpp
#include "PitchCurve.h"

#include <algorithm>
#include <cmath>

namespace spacedust
{
    void PitchCurve::clear()
    {
        points.clear();
    }

    void PitchCurve::addPoint (float t01, float semitones)
    {
        const float t = t01 < 0.0f ? 0.0f : (t01 > 1.0f ? 1.0f : t01);

        points.push_back (Point { t, semitones });

        std::sort (points.begin(), points.end(),
                   [] (const Point& a, const Point& b) { return a.t01 < b.t01; });
    }

    bool PitchCurve::isFlat() const noexcept
    {
        for (const auto& p : points)
            if (p.semitones != 0.0f)
                return false;

        return true;
    }

    float PitchCurve::valueAt (float t01) const noexcept
    {
        if (points.empty())
            return 0.0f;

        if (t01 <= points.front().t01)
            return points.front().semitones;

        if (t01 >= points.back().t01)
            return points.back().semitones;

        for (size_t i = 1; i < points.size(); ++i)
        {
            const auto& a = points[i - 1];
            const auto& b = points[i];

            if (t01 > b.t01)
                continue;

            const float span = b.t01 - a.t01;

            if (span <= 0.0f)
                return b.semitones;

            const float f = (t01 - a.t01) / span;
            return a.semitones + (b.semitones - a.semitones) * f;
        }

        return points.back().semitones;
    }
}
```

- [ ] **Step 5: Run the test to verify it passes**

Add `Source/PitchCurve.cpp` to the `mod_matrix_test` target's sources and add `#include "../../Source/PitchCurve.h"` to the test.

```bash
cmake --build build --config Release --target modmatrix-test
```

Expected: `All modulation matrix tests passed.`

- [ ] **Step 6: Replace the ramp in the voice**

Add `pitchCurveTime` to the end of `createParameterLayout`, range 0-10 s, default 0. Delete `pitchEnvAmount`, `pitchEnvTime` and `pitchEnvPitch`.

Replace `SynthVoice.cpp:1645-1667`:

```cpp
        double pitchForOscillators = currentPitch;
        bool   pitchEnvShapingNow  = false;

        // A flat curve is skipped entirely, so a patch that draws nothing costs
        // nothing and sounds exactly as it did.
        if (pitchCurve != nullptr && ! pitchCurve->isFlat()
            && pitchCurveTime >= 0.0001f && sampleRate > 0.0f)
        {
            const float elapsedSec = pitchEnvSamplesElapsed / (float) sampleRate;
            const float t01 = elapsedSec / pitchCurveTime;

            const float semitones = juce::jlimit (-48.0f, 48.0f, pitchCurve->valueAt (t01));
            const double ratio = std::pow (2.0, (double) semitones / 12.0);

            // Anchored to the intended note, not to the glide-tracking pitch, for
            // the same reason the old ramp was: rapid notes whose glide has not
            // finished would otherwise bend a meaningless mid-glide pitch.
            pitchForOscillators = targetPitch * ratio;
            pitchEnvShapingNow = (semitones != 0.0f);
        }

        if (pitchEnvSamplesElapsed < 1e7f)
            pitchEnvSamplesElapsed += 1.0f;
```

Add `const spacedust::PitchCurve* pitchCurve = nullptr;` and `float pitchCurveTime = 0.0f;` to `SynthVoice.h`, with a `setPitchCurve` that points every voice at the processor's one curve.

- [ ] **Step 7: Save, load, and migrate the curve**

Add `PITCHCURVE` to `getStateInformation` and `setStateInformation` beside `MODMATRIX`, following the same lift-out pattern. In `migrateLfoTargetsIfOld`, also convert the old values:

```cpp
    // The old three knobs made one straight fall from amount/100 * pitch
    // semitones to zero over time. Two points say the same thing.
    const float oldAmount = (float) readOld (state, "pitchEnvAmount", 0.0);
    const float oldPitch  = (float) readOld (state, "pitchEnvPitch",  0.0);
    const float oldTime   = (float) readOld (state, "pitchEnvTime",   0.0);

    if (oldAmount != 0.0f && oldPitch != 0.0f && oldTime > 0.0f)
    {
        curve.clear();
        curve.addPoint (0.0f, oldAmount / 100.0f * oldPitch);
        curve.addPoint (1.0f, 0.0f);
        setParameterValue (state, "pitchCurveTime", oldTime);
    }
```

- [ ] **Step 8: Commit**

```bash
git add Source/PitchCurve.h Source/PitchCurve.cpp Source/PluginProcessor.cpp Source/SynthVoice.h Source/SynthVoice.cpp tools/modmatrixtest/mod_matrix_test.cpp CMakeLists.txt
git commit -m "feat(pitch): a drawn curve in place of a straight ramp"
```

---

## Task 12: The pitch curve box and its editor

**Files:**
- Create: `Source/PitchCurveEditor.h`, `Source/PitchCurveEditor.cpp`
- Modify: `Source/PluginEditor.h`, `Source/PluginEditor.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `spacedust::PitchCurve` (Task 11), `WaveformEditorPanel`'s frame, `titleBarArea`, `clampInsideParent` (existing).
- Produces: `PitchCurveBox : public juce::Component` and `PitchCurveEditorPanel : public juce::Component`.

- [ ] **Step 1: Write the small box**

`PitchCurveBox` draws the curve at thumbnail size, flat by default, and calls a callback on click. It sits where Pitch Env Amount, Time and Pitch were.

```cpp
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
}
```

- [ ] **Step 2: Write the editor panel**

`PitchCurveEditorPanel` reuses `WaveformEditorPanel`'s frame, title bar drag and `clampInsideParent`, so it moves around the window the same way. Its plot area:

- Vertical axis -24 to +24 semitones, with a line at zero.
- Horizontal axis 0 to the Time knob.
- Click adds a point, drag moves it, right-click removes it.
- The Time knob sits underneath the plot.

- [ ] **Step 3: Replace the three knobs**

Remove `pitchEnvAmountSlider`, `pitchEnvTimeSlider`, `pitchEnvPitchSlider` and their labels and attachments from `PluginEditor.h` and `PluginEditor.cpp`. Put the `PitchCurveBox` in their place in the Amp Envelope layout.

- [ ] **Step 4: Verify nothing is left**

```bash
grep -rn "pitchEnvAmount\|pitchEnvPitch\|pitchEnvTime" Source/
```

Expected: no output outside the migration in `PluginProcessor.cpp`.

- [ ] **Step 5: Build, deploy, and check**

Confirm: the box shows a flat line on a new patch; clicking opens a panel that can be dragged around the window; drawing a fall from +12 to 0 with Time at 0.2 s gives an audible pitch attack; and the Standalone and VST3 are both deployed.

- [ ] **Step 6: Run everything**

```bash
cmake --build build --config Release --target modmatrix-test
cmake --build build --config Release --target unison-test
cmake --build build --config Release --target notelock-test
cmake --build build --config Release --target SpaceDustChunkAudit
./run-pluginval.ps1
```

- [ ] **Step 7: Commit**

```bash
git add Source/ CMakeLists.txt
git commit -m "feat(pitch): draw the curve in a window you can move"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
| --- | --- |
| 1. The modulation matrix | 1, 2 |
| 2. Delivery, per sample | 3, 4 |
| 3. Assign mode | 5, 6 |
| 4. The indicator bar | 5, 7 |
| 5. LFO 3 and LFO 4 | 9 |
| 6. What is deleted | 8, and 11 for the Pitch Env knobs |
| 6. Migration | 10, and 11 for the curve |
| 7. The drawn pitch curve | 11, 12 |
| 8. Saving and loading | 2, and 11 for PITCHCURVE |
| 9. Testing | 1, 3, 11, and 12 for the full run |
| 10. Order of work | Tasks 1-12 follow the five commits |

Every spec requirement has a task. The destination sweep named in spec section 9 is Task 4 steps 7 and 8.

**Type consistency:** `spacedust::numLfos`, `ModRouting`, `DestRange`, `CompiledRouting`, `ModMatrix::applyByName`, `ModMatrix::applyCompiled`, `DestinationTable::slotFor`, `AssignModeState::colourFor`, `ModulatableKnob::setLfoPhases`, `PitchCurve::valueAt` are used with the same names and signatures everywhere they appear.
