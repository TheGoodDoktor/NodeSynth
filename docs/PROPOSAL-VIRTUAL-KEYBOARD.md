# Virtual Keyboard Node — Technical Proposal

## Goal

A `FVirtualKeyboard` node that lets you drive the synth from mouse + computer keyboard while a real MIDI controller isn't plugged in. One-octave layout, octave shift, and a mod-wheel slider. Drop-in replacement for `FMidiInput` in the existing target patch.

## Output ports

Mirror `FMidiInput` order so an existing patch can swap source nodes by re-routing two links instead of all of them. Add ModWheel as a 4th port:

| # | Name | Type | Range |
|---|---|---|---|
| 0 | Gate | Control | 0 / 1 |
| 1 | Frequency | Control | Hz |
| 2 | Velocity | Control | 0..1 |
| 3 | ModWheel | Control | 0..1 |

ModWheel is new — `FMidiInput` doesn't expose CC1 yet. The proposal is to add it here first; lifting the same output into `FMidiInput` (CC1 handler) is a small follow-up so both sources stay symmetric.

## Parameters

| Param | Kind | Notes |
|---|---|---|
| Octave | Float (or Int-as-Float) | Range -2..+5, default +4. Shifts the displayed octave; bottom key C maps to MIDI note `12 * (Octave + 1)`. |
| Velocity | Float | 0..1, default 0.8. Fixed velocity for clicked/typed notes (no velocity sensitivity). |
| ModWheel | Float | 0..1, default 0. Smoothed via `FOnePoleSmoother` like other slider params. |

Octave kept as a param (rather than transient UI state) so it's serializable when patch save/load lands in Phase 3.

## UI — drawn in the property panel

`FGateButton` is the precedent for "node = interactive UI control": all interaction lives in `DrawPropertyPanel`. For the keyboard we need custom drawing beyond standard widgets, so:

- Add an optional virtual `void DrawCustomUI()` on `INode` (default no-op). `DrawPropertyPanel` calls it after the standard param loop.
- `INode.h` itself stays free of ImGui — the override lives in `VirtualKeyboard.cpp`.

**Layout in the panel:**
- One-octave piano row (13 keys C..C, ~24px white / black overlay), drawn with `ImGui::InvisibleButton` + custom `DrawList` rectangles. Detect press/release per key.
- `[ Octave - ]  C4  [ Octave + ]` row above, `Mod` vertical slider to the left of the keys.
- Key labels showing the computer-keyboard mapping (FL/Ableton convention):
  - White: `A S D F G H J K` → C D E F G A B C
  - Black: `W E _ T Y U _` → C# D# F# G# A#
- Computer-keyboard polling gated on `ImGui::IsWindowFocused(RootAndChildWindows) && !GetIO().WantTextInput` so it doesn't fight text fields.

## Mouse interaction

Mouse clicking is the primary input path; computer-keyboard mapping is an extra.

- Each piano key is an `ImGui::InvisibleButton` covering its rectangle (white keys 24px wide, black keys overlaid on top and queried first so they win the hit test).
- **Mouse-down on a key** → push that note onto the held-note stack, recompute gate/freq, store atomics. Gate goes high, the note plays.
- **Mouse-up (or drag off the key)** → pop the note. If nothing else is held, gate goes low.
- **Drag across keys with the button held** → as the mouse enters a new key, release the old one and press the new one (glissando). Detected via `ImGui::IsItemHovered()` + `ImGui::IsMouseDown(0)` per key.
- DrawList fill colour switches to a "pressed" tint while held for visual feedback.

Minimum interaction: select the node, click a key.

## Threading

No SPSC ring needed. Unlike `FMidiInput` (RtMidi callback runs on its own thread, events arrive at unpredictable times and can cluster), virtual-keyboard input is produced at human/UI-frame rates. Atomic state suffices:

```cpp
std::atomic<int32_t>  CurrentNote{ -1 };   // -1 = no note held
std::atomic<float>    CurrentFreq{ 0.0f };
std::atomic<float>    CurrentVelocity{ 0.0f };
std::atomic<bool>     bGate{ false };
std::atomic<float>    ModWheel{ 0.0f };    // raw target; smoother lives in audio thread
int32_t               OctaveShift;          // UI thread only; folded into CurrentNote at write time
```

UI thread maintains a small fixed-capacity held-note stack (same `MaxHeldNotes = 16` pattern as `FMidiInput`) — multiple computer-keyboard keys can be held simultaneously. On any change to the stack, recompute (note, gate) and store atomically.

Audio thread `Process()`:
- Reads `bGate`, `CurrentFreq`, `CurrentVelocity` once per block, writes constant blocks.
- Reads `ModWheel` once per block into the smoother, writes per-sample smoothed output (matches `FGain::Gain` pattern).

This keeps the node fully RT-safe (no allocations, no locks, no `shared_ptr` traffic) and inherits the project's existing "atomic-for-params, sample-accuracy is Phase 3+" stance.

## Note priority

Last-note-wins with legato, matching `FMidiInput` and `FAdsr`'s behaviour:
- Press: push note onto the stack, gate goes/stays high, freq jumps to new note.
- Release: pop matching note from anywhere in the stack. If stack now non-empty, freq drops to the new top, gate stays high. If empty, gate goes low.

Re-using `FMidiInput`'s pattern means `FAdsr`'s legato logic Just Works.

## Files to add / change

| File | Change |
|---|---|
| `src/dsp/VirtualKeyboard.h` (new) | Class declaration, atomics, params. |
| `src/dsp/VirtualKeyboard.cpp` (new) | `Process`, `DrawCustomUI` (the only place we touch ImGui in the dsp layer). |
| `src/dsp/Node.h` | Add `virtual void DrawCustomUI() {}` to `INode`. |
| `src/ui/Editor.cpp` | Include header; call `Rec->Node->DrawCustomUI()` after the param loop in `DrawPropertyPanel`; add menu entry "Virtual Keyboard" to the create-node popup. |
| `CMakeLists.txt` | Append the two new sources to the `nodesynth` target list. |
| `tests/VirtualKeyboard.test.cpp` (new) | See below. |

No changes to `FGraphModel`, `FAudioGraph`, `FMidiRing`, or any existing DSP node.

## Tests

Catch2 tests, headless (no ImGui needed — drive the node via `SetParamValue` and a synthetic "press/release" helper that mutates the held-note stack the same way the UI would):

- Press one note → gate=1, freq matches `440 * 2^((note-69)/12)` within 1e-3.
- Press, release → gate=0.
- Press A, press B (held), release A → gate stays 1, freq = B's freq (last-note-wins).
- Octave shift changes output freq while a note is held (legato across octave).
- ModWheel smoother converges to target within N samples (mirror `Smoother.test.cpp` style).
- `Process()` with `bGate=false` writes zero gate without touching freq output (defined behaviour for downstream nodes).

## Open questions / explicit non-goals

- **No sustain.** Skipping for v1; the `FAdsr` release does the job. Easy to add later as a Bool param OR'd into the gate.
- **No pitch-bend.** Real MIDI keyboards have it, but it's a separate output port and `FMidiInput` doesn't expose it either. Tackle alongside the `FMidiInput` CC1/bend uplift.
- **No "stuck note" handling on focus loss.** If the property-panel window loses focus while a computer-key is held, the OS won't deliver the keyup. Mitigation: in `DrawCustomUI`, when `IsWindowFocused` flips false and any computer-keys are held, force-release them. Worth implementing in v1 — one-line fix that prevents an obvious foot-gun.
- **In-node piano (instead of property-panel-only).** Possible later by extending `INode` with a `DrawNodeBody()` hook the editor calls between `BeginNode`/`EndNode`, but that's a bigger UI change and not required to meet the stated goal.

## Estimate

~250 LOC of node code + ~50 LOC editor wiring + ~150 LOC tests. Roughly half a day if the property-panel hook lands cleanly; the piano-key drawing is the only fiddly part.
