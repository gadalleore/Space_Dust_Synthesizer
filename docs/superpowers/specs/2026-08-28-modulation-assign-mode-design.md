# Modulation Assign Mode, Four LFOs, and a Drawn Pitch Curve

Design agreed with Giuseppe, 2026-08-28.

## Why

Today an LFO can reach exactly six destinations, chosen from one drop-down.
Everything else in the plugin is fixed while a note sounds. Two extra "mod
filters" exist on the Modulation page only because an LFO had no other way to
reach a filter.

After this change any continuous knob in the plugin is a destination, chosen by
pointing at the knob itself. The mod filters lose their reason to exist, and the
space they held becomes LFO 3 and LFO 4.

## What is being built

1. A modulation matrix with no fixed size, saved in the patch.
2. Per-sample delivery of every routing, including routings onto effects.
3. An assign mode: press Assign, the knobs light up, drag one to set an amount.
4. A live indicator bar beside every knob that has a routing.
5. LFO 3 and LFO 4.
6. Deletion of Mod Filter 1 and 2, and of both Destination drop-downs.
7. A drawn pitch curve that replaces the three Pitch Env knobs.

## 1. The modulation matrix

New files: `Source/ModMatrix.h` and `Source/ModMatrix.cpp`. JUCE types are used
only where the API needs them, so the arithmetic can be tested without building
a plugin. `Source/NoteLockGrid.*` is the precedent.

### The routing

```cpp
struct ModRouting
{
    int          lfoIndex;     // 0..3
    juce::String destination;  // an APVTS parameter id
    float        amount;       // -1.0 .. +1.0
};
```

The matrix holds a `std::vector<ModRouting>`. There is no limit on the count,
which is why it is stored in the patch and not as parameters.

At most one routing exists for a given (lfoIndex, destination) pair. Assigning
the same knob to the same LFO twice replaces the amount rather than adding a
second entry. Two different LFOs may both reach one knob; their contributions
add before the clamp.

### What a destination may be

A parameter qualifies as a destination when all of these hold:

- It is an `AudioParameterFloat`. Bools and Choices are excluded, because a
  wobble between "on" and "off" is a different feature.
- Its id does not begin with `lfo`. An LFO may not modulate any LFO control,
  including its own. This is what makes assign mode unnecessary on the
  Modulation page.
- It is not `pitchBend`, which the host drives.

The list is built once at construction by walking the APVTS parameter list, so
a parameter added later becomes assignable with no extra work.

### The value

For one destination:

```
modulated = base + sum over routings of (amount * lfoValue * halfRange)
final     = clamp(modulated, range.start, range.end)
```

where `base` is the knob's own current value and `halfRange` is
`(range.end - range.start) * 0.5`.

`lfoValue` is **already scaled by that LFO's Depth knob**, which is kept and
still applies to every one of its routings. The routing amount is a further trim
on top. Depth is therefore the master level for one LFO, and amount is the
balance between the knobs it reaches. Nothing may apply Depth twice.

`base` being the knob value is what "the LFO rotates around the current setting
of the knob" means. Move the knob and the whole movement moves with it. An
amount of +100% with the LFO at its peak reaches half the knob's full travel
above the knob, which the clamp then holds inside the legal range.

## 2. Delivery, per sample

### Voice destinations

The voice already runs a per-sample loop. The matrix hands it a block of
modulated values, one per modulated destination per sample.

The six destinations that exist today — pitch, filter cutoff, master volume,
Osc 1 volume, Osc 2 volume, noise volume — keep the exact code paths they use
now, in `SynthVoice.cpp` around line 1700. Nothing about the sound of an
existing patch changes.

### Effect destinations

Every effect takes a whole `juce::AudioBuffer&`, nothing in the audio path uses
an FFT, and only the reverb declares a maximum block size. So the effects chain
gains a chunk loop rather than a rewrite.

- Chunk size is 32 samples, giving a control rate near 1400 Hz at 44.1 kHz.
- Each chunk is a `juce::AudioBuffer` view onto the same memory, built with the
  `(float* const*, int, int, int)` constructor. No copying.
- Between chunks, every modulated effect parameter is written from the matrix.
- **When a patch modulates no effect parameter, the chain runs as one whole
  block, exactly as it does today.** This is not an optimisation to add later;
  it is what keeps existing patches free of any cost and free of any change.

Chunking makes blocks smaller than the declared maximum, so the reverb is safe.

**Known risk.** `SpaceDustTranceGate::process` is handed the playhead and works
out its own position in the bar. Called on chunks it would re-read the same
position 16 times per block and its grid would drift. Its signature must take a
sample offset, and the fix must be proved by a test that renders a gate pattern
chunked and whole and compares the two.

## 3. Assign mode

### Entering and leaving

Each LFO panel gains an **Assign** button. Pressing it puts the editor into
assign mode for that LFO.

While assign mode is on, a highlighted **Exit LFO Mode** button appears in the
tab strip, beside the tab buttons. Pressing it leaves the mode. Pressing Escape
does the same.

Assign mode is available on every tab except Modulation, because no LFO control
is a legal destination. Switching to the Modulation page while assign mode is on
hides the highlighting but does not leave the mode, so you can go back to
another tab and carry on.

### Assigning

Every assignable knob draws a coloured ring while the mode is on. The colour is
the LFO's own:

| LFO 1 | LFO 2 | LFO 3 | LFO 4 |
| --- | --- | --- | --- |
| blue | green | amber | magenta |

Dragging a ringed knob sets the amount for that (LFO, knob) pair. Up is
positive, down is negative, and the range is -100% to +100%. A drag through zero
removes the routing rather than leaving a dead entry.

While a drag is running, a small percentage reads out at the top right of that
knob.

A ringed knob does not change its own value in assign mode. The drag sets the
routing amount only.

### Implementation

A `ModulatableKnob` wrapper, not 150 edited call sites. It owns the ring, the
percentage readout, the drag handling and the indicator bar, and it defers to
the wrapped `juce::Slider` when assign mode is off. `PluginEditor.cpp` is
already 8588 lines; this must not add 150 more.

## 4. The indicator bar

A thin vertical bar sits to the right of any knob that has at least one routing.
It is drawn whether or not assign mode is on, so what moves in a patch is always
visible.

- The filled extent of the bar is the amount.
- A bright marker inside the bar shows where that LFO is in its cycle now.
- The bar carries the colour of the LFO that owns the routing. Where two LFOs
  reach one knob, the bar is split into one lane each.

The marker is driven by one `std::atomic<float>` per LFO, published by the
processor once per block, and read by a 30 Hz repaint timer in the editor. The
timer runs only while at least one bar is on screen.

Clicking or dragging the bar changes the amount without entering assign mode.

## 5. LFO 3 and LFO 4

Nine parameters each, copying LFO 1 and 2 exactly, minus the deleted Target:

```
Waveform, Enabled, Depth, Sync, Rate, Phase,
TripletEnabled, TripletStraightToggle, Retrigger
```

They take the space Mod Filter 1 and 2 leave on the Modulation page. The four
LFO panels share one layout function rather than four near-identical copies.

## 6. What is deleted

| Removed | Count | Where |
| --- | --- | --- |
| Mod Filter 1 and 2 parameters | 16 | `PluginProcessor.cpp` 4102-4195 |
| Mod filter per-voice DSP | — | 84 references in `SynthVoice.cpp` |
| Mod filter UI | — | 230 references in `PluginEditor.cpp` |
| `lfo1Target`, `lfo2Target` | 2 | the Destination drop-downs |
| `pitchEnvAmount`, `pitchEnvTime`, `pitchEnvPitch` | 3 | see section 7 |

Parameter count: 291 today, minus 21, plus 19 added, is **289**.

### Migration

State version rises to the next number. `migrateLfoRatesIfOld` is the pattern.

- An old `lfo1Target` or `lfo2Target` becomes a routing in the new matrix, with
  the amount set to **+1.0**, not to that LFO's Depth. Depth is a parameter that
  survives the migration untouched and already scales the LFO output; taking the
  amount from it as well would square the depth and halve the movement of every
  migrated patch. Saved patches keep their movement exactly.
- The mod filters have no equivalent and **cannot** be migrated. A patch that
  used one loses that filter. This was accepted.
- Old `pitchEnv*` values become a two-point curve that falls from
  `amount / 100 * pitch` semitones to zero over `time`, which is what the
  current ramp does. Saved patches keep their pitch attack.

### Accepted consequence

Removing 21 parameters shifts the VST3 index of every parameter after them.
Automation recorded in a host against a V2 project will point at the wrong knob.
V2 is not released, and this was accepted.

## 7. The drawn pitch curve

Replaces the three Pitch Env knobs in the Amp Envelope.

- A small box in their place draws the curve, flat by default.
- Clicking the box opens a movable editor panel that reuses
  `WaveformEditorPanel`'s frame, title bar drag and `clampInsideParent`.
- Vertical axis: semitones, -24 to +24. Horizontal axis: 0 to the Time knob.
- One new parameter, `pitchCurveTime`, 0-10 s, replacing `pitchEnvTime`.
- The curve is a list of points, saved in the patch beside the matrix.
- At the end of the curve the last value is held. A curve drawn back to zero
  behaves exactly as today's ramp does, which keeps the default unchanged.
- The curve retriggers at note-on, as `pitchEnvSamplesElapsed` does now.

## 8. Saving and loading

Two new children of the state XML, `MODMATRIX` and `PITCHCURVE`.

They follow the `USERWAVES` pattern already in `setStateInformation`: lifted out
of the XML **before** `juce::ValueTree::fromXml`, then restored after
`apvts.replaceState`. Left in place they would become part of the parameter tree
and be written out again on every save, doubling in size each round trip.

## 9. Testing

| Test | Proves |
| --- | --- |
| `tools/modmatrixtest` | bipolar amounts, clamping, two LFOs summing on one knob, unknown ids ignored, save and load round trip |
| Chunk equality run | with nothing modulated, the chunked effects chain is **bit-identical** to today. If it is not, the chunking is wrong. |
| Trance gate chunk test | a gate pattern rendered in chunks matches the same pattern rendered whole |
| Destination sweep | drives the real processor with one LFO on a new destination and measures that the output actually moves |
| Migration test | an old patch loads with its LFO destination and its pitch attack intact |

The chunk equality run is the one that matters most. Everything in section 2
rests on it.

Every tool that builds a processor must use the `LiveStateGuard` from
`tools/unisonaudit`, or it will publish its sweep values into `current.sdpreset`
and overwrite the loaded patch.

## 10. Order of work

Five commits, each building and testing on its own so `git bisect` stays useful.

1. `ModMatrix` and its tests. No wiring. Nothing in the plugin changes.
2. Per-sample delivery and effect chunking, including the trance gate offset.
   Proved bit-identical before anything can be assigned.
3. Assign mode, `ModulatableKnob`, the indicator bar, Exit LFO Mode.
4. Delete the mod filters and both Destination drop-downs; add LFO 3 and 4;
   write the migration.
5. The drawn pitch curve.
