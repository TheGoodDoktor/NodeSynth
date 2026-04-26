# NodeSynth — Phase 4 Development Plan

Expands on Phase 4 of `docs/PLAN.md`. Entry state: Phases 0–3 complete. SPSC `SetParam` / `NoteOn` / `NoteOff` queue. Polyphonic compile-time partition with per-voice cloning + voice-mixer synthesis. JSON patch save/load with optional native-file-dialog still pending. 104 Catch2 tests / 21k+ assertions green.

**Phase 4 goal:** the synth feels like a real instrument — time-domain effects, a step sequencer to drive it, in-graph metering for debugging, and undoable edits in the editor.

**Final deliverable that defines "done":** a polyphonic patch with `Sequencer → VoiceAllocator → Osc (per-voice) → Filter (per-voice) → Delay → Reverb → Output`, with a Scope tapped off the master bus, runs continuously without glitches; the user can audition tweaks and undo/redo every model change with `Ctrl+Z` / `Ctrl+Y`.

---

## 1. Decisions to lock first

### 1.1 Delay buffer strategy

Phase-3 nodes never allocate inside `Process`. Phase 4 doesn't break that invariant.

**Decision:** every effect node that needs persistent buffer memory (delay, reverb, scope, meter) allocates a fixed-size buffer in `Prepare(SampleRate)`. The compile-time max for `FDelay` is **2 seconds at sample rate** (96000 samples at 48 kHz, ~370 KiB per node — acceptable). Modulation of the delay time within that range is via a Control input + linear-interpolated read tap.

### 1.2 Reverb topology

**Decision:** **Freeverb** (8 lowpass-feedback comb filters → 4 series allpass diffusers). Stable across the full param range, public-domain reference, mono-in / mono-out for v1. Stereo is a Phase 5 concern. Convolution and plate-style reverb stay out of scope.

### 1.3 Waveshaper repertoire

`FWaveshaper` exposes a `Shape` Choice param: **TanhSoft / HardClip / SoftClip / Asymmetric** (fold). Drive (pre-gain in dB) and Output (post-gain in dB) Float params.

**Aliasing note:** all of these alias at high drive when the oscillator's harmonics fold past Nyquist. Oversampling lands in Phase 5; for now, document the limitation.

### 1.4 Sequencer model

```
FSequencer  inputs : Clock  (Control, rising edge advances step)
                     Reset  (Control, rising edge → step 0)
            outputs: Gate    (Control, 1 during the active step's gate-length window)
                     Note    (Control, Hz of the active step's pitch)
                     Velocity (Control, 0..1)
            params : NumSteps (1..16, default 16)
                     Step[i] : Pitch (semitones from Param_RootNote), Velocity, GateLength (0..1), Enabled
                     RootNote (MIDI note for step pitch=0; default 60 = C4)
```

External clock only. No internal BPM oscillator in v1 — drive it with an LFO (square wave) or `FConstant + FOscillator` if you need a tempo; simpler to start with a dedicated `Clock` node (a Phase 4 mini-deliverable, see 4D).

Per-step state lives in `std::atomic<float>` arrays so the property-panel grid edits flow through the existing SetParam queue.

### 1.5 Scope + meter buffer policy

`FScope` and `FMeter` are *taps* — Audio in, Audio passthrough out (so they can be inserted anywhere and chained). They write samples to a fixed-size SPSC ring (audio → UI thread); the UI snapshots once per frame for rendering. No locks, no allocation in `Process`.

- `FScope`: ~1024-sample window (~21 ms at 48 kHz). User-tunable via `WindowSize` param.
- `FMeter`: tracks peak + RMS over a short window (~50 ms). Property panel draws bar + numeric readout.

### 1.6 Undo/redo command shape

```cpp
enum class EEditCommand : uint8_t
{
    AddNode, RemoveNode,
    AddLink, RemoveLink,
    SetParam,
    SetNodePerVoice,
    SetNodePosition,
};

struct FEditCommand { /* enough state to redo + undo */ };
```

Each command captures both forward and reverse data — `SetParam` stores `OldValue` and `NewValue`; `RemoveNode` captures the node's full state (type, params, position, per-voice flag, all incident links) for reconstruction.

Stack: linear undo + redo. New user action drops the redo stack. Cap at **200 entries** (memory bound; serialised node state is small).

**Slider coalescing:** a single drag of a slider should produce *one* undoable edit, not 100. Use ImGui's `IsItemDeactivatedAfterEdit()` as the commit point — record the SetParam only when the user releases the slider, with the value-on-press as the OldValue.

**Hotkeys:** `Ctrl+Z` / `Ctrl+Y` (and `Ctrl+Shift+Z`) gated on `!ImGui::GetIO().WantTextInput` so they don't fire while typing in a field or filename popup.

### 1.7 Clock node

A small `FClock` source emits a square-wave Gate output at a configurable BPM. Drives the sequencer's Clock input out-of-the-box without needing the user to wire an LFO + Multiply themselves. Ships as part of 4D since it's the natural test driver.

### 1.8 Stereo

**Out of scope.** Phase 5 deliverable. Every Phase-4 node stays mono-in / mono-out. Document explicitly so Phase-5 work doesn't rediscover this.

### 1.9 Voice-mixer gain convention

`Internal::FVoiceMixer` (the compiler-synthesised summing node) currently does a straight sum of N voice buffers — no attenuation. With `NumVoices=8` and per-voice envelopes at `Sustain=0.7`, in-phase chords can peak at `8 × 0.7 = 5.6`, which clips at the device output. The existing seeded patch compensates with a master `Gain` of 0.15 downstream of the mixer; users authoring polyphonic patches need to do the same.

**Decision (Phase 4):** keep the "straight sum, user trims downstream" model. Don't make `FVoiceMixer` auto-divide by `NumVoices`.

Why not auto-attenuate:
- Audio summing is what the user expects from a "polyphonic mix" — a single voice should sound at its full amplitude, not at 1/N of it. Auto-divide changes the loudness of every existing patch silently when reloaded, and makes per-voice authoring counter-intuitive (you'd write a voice that sounds quiet in isolation and only "fills in" when other voices play).
- The Freeverb / Delay / Waveshaper effects in 4A–4C want to see signals scaled the same way regardless of how many voices are active — auto-divide breaks that.
- `FVoiceMixer` is internal; users can't insert a custom mixer between voices and the boundary if they want different summing behaviour. So a hardcoded division is the wrong place to bake a policy.

**How to apply:** a polyphonic patch authoring guideline — put a `Gain` (or `VCA`) on the master bus and trim it for the chord density you expect (`1/N` is a clip-free starting point; many users prefer `1/√N` and accept rare clips). Document this in `CLAUDE.md` so future seeded patches and tutorials get it right. Auto-attenuation stays in the deferred list (see §5) in case real-world feedback flips the call.

---

## 2. Sub-phases

```
4A (Delay)        ──┐
4B (Reverb)       ──┤
4C (Waveshaper)   ──┼──► 4F (Undo/redo) ──── done
4D (Sequencer)    ──┤
4E (Scope/Meter)  ──┘
```

4A–4E are independent; pick any order. 4F goes last because it wraps every `FGraphModel` mutator and benefits from all of the new node types being in the registry first.

### Phase 4A — Delay (~2 days)

- `src/dsp/Delay.h` — `FDelay` (`TNodeBase<2, 1>`): Audio in, Time Control input (Hz, used as ms), Audio out.
- Params: `TimeMs` (1..2000, log), `Feedback` (0..0.95 — capped to prevent runaway), `Tone` (one-pole damping in feedback path, 0..1).
- Pre-allocates `MaxDelaySamples = 2.0 * SampleRate` ring buffer in `Prepare`. Tests confirm no audio-thread allocation by checking buffer pointer is non-null after Prepare and unchanged across Process calls.
- Read tap uses linear interpolation between adjacent samples for smooth modulation.
- Catch2 tests: impulse at sample 0 emerges at sample N (where N = `TimeMs * SampleRate / 1000`), feedback gain matches param, tone param attenuates highs.
- Icon: a Λ shape with a curved feedback arrow.

### Phase 4B — Reverb (~3 days)

- `src/dsp/Reverb.h` — `FReverb` (`TNodeBase<1, 1>`). Internal Freeverb implementation: 8 lowpass-feedback combs + 4 series allpass.
- Params: `RoomSize` (0..1), `Damping` (0..1), `Wet` (0..1) — dry mix is `1 - Wet` so the user gets blend without wiring an external mixer.
- Comb buffers pre-allocated in `Prepare`. Standard Freeverb tunings (delay lengths in samples scaled to the configured sample rate).
- Catch2 tests: impulse decays to silence over ~RoomSize seconds, output stays bounded (no NaN/Inf at any param extreme).
- Icon: concentric arcs / ripple pattern.

### Phase 4C — Waveshaper / distortion (~1 day)

- `src/dsp/Waveshaper.h` — `FWaveshaper` (`TNodeBase<1, 1>`).
- Params: `Shape` (Choice), `Drive` (0..40 dB), `Output` (-20..20 dB).
- Stateless per-sample function — no buffer state, no smoother needed beyond standard `Drive` smoothing to avoid zipper.
- Tests: known input matches expected output for each shape; Drive and Output applied correctly.
- Icon: a hand-drawn S-curve clip line.

### Phase 4D — Sequencer + Clock (~3 days)

- `src/dsp/Clock.h` — `FClock` (`TNodeBase<0, 1>`): emits a 50%-duty-cycle Gate at configurable BPM. Param: `Bpm` (1..400). Internal phase accumulator.
- `src/dsp/Sequencer.h` — `FSequencer` (`TNodeBase<2, 3>`).
- Per-step storage: `std::array<std::atomic<float>, 16>` for each of Pitch / Velocity / GateLength / Enabled.
- Property panel: 16-cell grid widget. Each cell shows pitch (vertical drag) + velocity (cell brightness) + enabled (click toggles). Custom drawing in a new `src/ui/SequencerUI.{h,cpp}` (parallel to `VirtualKeyboardUI`, `AdsrUI`).
- Edits push SetParam through the queue (ParamIndex encodes step + sub-field; e.g. `Step_Pitch_Base + i`).
- Tests: rising-edge clock advances step; reset jumps to 0; gate-length 0.5 yields gate-high for first half of step duration; disabled step skips its gate-high window.
- Icons: clock = clock face, sequencer = grid of vertical bars.

### Phase 4E — Scope + Meter (~2 days)

- `src/dsp/Scope.h` — `FScope` (`TNodeBase<1, 1>`, audio passthrough). Internal SPSC ring of 4096 samples (audio writes, UI reads). Property-panel custom UI draws a polyline of the most recent `WindowSize` samples.
- `src/dsp/Meter.h` — `FMeter` (`TNodeBase<1, 1>`, audio passthrough). Tracks peak (instant max abs) and RMS (50-ms window) as `std::atomic<float>`. Property-panel custom UI draws a stereo-style bar with peak hold + numeric dB readout.
- New ring template `TSpscScopeRing<size_t Capacity>` in `src/midi/` (or a new `src/util/` directory) — same shape as the audio command and MIDI rings.
- Tests: scope captures a known sine signal correctly; meter peak matches the input's peak amplitude; RMS matches expected for a known sine.
- Icons: scope = oscilloscope trace, meter = vertical bar.

### Phase 4F — Undo/redo (~3-4 days)

- `src/graph/EditHistory.{h,cpp}`: `FEditCommand` variant + `FEditHistory` class with `Push`, `Undo`, `Redo`. Linear stack capped at 200 entries. New push drops the redo stack.
- Hook every `FGraphModel` mutator (`AddNode`, `AddNodeWithId`, `RemoveNode`, `AddLink`, `RemoveLink`, `SetNodePerVoice`, plus a new `SetNodePositionWithUndo` that captures both old and new positions).
- `SetParam` coalescing: `Editor.cpp::DrawPropertyPanel` records the param's value-at-active when `IsItemActivated()` returns true, and records the edit when `IsItemDeactivatedAfterEdit()` returns true.
- Hotkeys: `Ctrl+Z` / `Ctrl+Y` / `Ctrl+Shift+Z` checked once per frame in `main.cpp`, gated on `!IO.WantTextInput`.
- File-menu `Edit` menu (or a small toolbar) with **Undo** / **Redo** items showing the next-action label (e.g. "Undo Add Oscillator").
- Loading a patch clears the history. New, empty model → empty history.
- Tests: AddNode → Undo restores empty model; SetParam → Undo restores prior value; Undo + Redo round-trips; mid-undo edit drops the redo stack; capacity cap evicts oldest.

---

## 3. Sequencing & budget

```
Day 1-2:    4A (Delay)
Day 3-5:    4B (Reverb)
Day 6:      4C (Waveshaper)
Day 7-9:    4D (Sequencer + Clock)
Day 10-11:  4E (Scope / Meter)
Day 12-15:  4F (Undo / Redo)
```

Total: **~14-15 focused days**. The sequencer's grid UI and the undo/redo's mutator-wrapping are the two biggest engineering chunks. Reverb is straightforward DSP if you stick to Freeverb.

If schedule pressure hits, the sensible cut is **4F**: ship Phase 4 minus undo/redo as Phase 4.0, defer 4F to 4.1. Effects + sequencer are the user-visible features; undo is quality-of-life.

---

## 4. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Delay-time modulation produces zipper noise | One-pole smoother on `TimeMs` (existing `FOnePoleSmoother`). Document max safe modulation rate (~50 Hz before audible artefacts). |
| Reverb explodes at extreme RoomSize / Damping combinations | Clamp internal feedback gains to <1.0; sanity test sweeps the param range checking for NaN/Inf. |
| Waveshaper aliases audibly at high Drive | Document. Oversampling lands in Phase 5. The audible artefact is a known property of memoryless nonlinear shapers, not a bug. |
| Sequencer step-edit UX is fiddly with 16 columns at hi-DPI | Allow horizontal scroll if the palette is narrower than the grid. Per-step popup for precise pitch entry as a fallback. |
| Scope ring overruns when audio is faster than UI redraw | Sized at 4096 samples = ~85 ms at 48 kHz; UI runs at 60 fps so ~3.5 KiB of new data per frame. Consumer drains in batches; producer drops on overflow (graceful — scope shows a discontinuity, not a crash). |
| Meter peak-hold flickers visually | Hold peak for ~500 ms before decaying, similar to hardware VU meters. Keep RMS smoothed over a longer window than the peak so the bar doesn't twitch every frame. |
| Undo for SetParam fires hundreds of times during a slider drag | Coalescing via `IsItemActivated` / `IsItemDeactivatedAfterEdit` (§1.6). Catch2 test: drive a slider through 100 intermediate values; assert undo stack gained exactly 1 entry. |
| Removing a node mid-undo breaks subsequent SetParam undo entries that target the now-defunct id | RemoveNode-undo entries capture the full node state for reconstruction with the same id (`AddNodeWithId`). Test: remove node, then undo, then verify a SetParam undo entry from before the removal still applies. |
| Redo after a parallel structural edit is dangerous (stale ids) | New user action drops the redo stack — eliminates this case by construction. |
| Undo entries pile up and OOM | 200-entry cap with FIFO eviction. Each entry's worst case (a `RemoveNode` for a sequencer with 16 steps × 4 fields) is ~500 bytes. ~100 KiB ceiling. |
| Hotkey conflict with text inputs (e.g. file-save dialog typing Ctrl+Z) | Gate hotkey handling on `!IO.WantTextInput`. |
| 4F's mutator wrapping bleeds into other code paths (e.g. patch load shouldn't generate undo entries) | Bypass-flag on `FGraphModel`. `LoadPatch` sets it during the rebuild, clears it after; mutators inside that window skip the history push. |

---

## 5. What stays deferred

- **Auto-attenuating voice mixer.** `Internal::FVoiceMixer` could divide its summed output by the number of active voices (or by `√N`) so the master output stays at unity regardless of chord density. Decided against in §1.9 — changes patch loudness silently on reload and breaks effect-bus expectations. Revisit if user feedback flips the call.
- **Stereo signal path.** Every Phase-4 node is mono. Phase 5 deliverable.
- **Oversampling** for nonlinear nodes (Waveshaper, future distortion variants). Phase 5.
- **Convolution / plate / hall reverb.** Phase 5+; Freeverb covers v1.
- **Tap-tempo / time-stretch on `FDelay`.** Phase 5.
- **VST3 wrapper.** Phase 5+ (large, optional).
- **Sample-accurate MIDI.** Still quantising to sample 0 of the drain block. Carries forward from Phase 3.
- **Structural commands through the SPSC queue.** Still snapshot-swap. Carries forward.
- **Polyphonic cables** (alternative voice model). Phase 3 chose the per-voice clone approach; revisit only if it shows fundamental limitations.
- **Live re-voicing** without recompile. Carries forward.
- **MPE.** Carries forward.
- **Preset browser.** Phase 5 polish.
- **History/Edit menu showing a list of recent actions** (per-action mini-icons in a panel). Hotkeys + menu entries are enough for v1; visual history is Phase 5 polish.
- **External clock sync** (MIDI clock, Ableton Link, ReWire). Out of scope for Phase 4.
- **Native file dialog** for File → Open / Save As. Still using the typed-path popup. Add `nfd-extended` whenever it bothers you enough.
