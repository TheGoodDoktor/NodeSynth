# NodeSynth — Phase 2 Development Plan

Expands on Phase 2 of `docs/PLAN.md`. Entry state: Phases 0 and 1 complete — graph-driven synth with sine `FOscillator`, `FGain`, `FOutput`, working node editor, atomic snapshot swap.

**Phase 2 goal:** user can build a subtractive mono synth and play it from a MIDI keyboard.

Final patch that defines "done":

```
MidiIn.freq  ──► Osc.Freq
MidiIn.gate  ──► ADSR ──► VCA.ctrl
               Osc ──► SVF ──► VCA ──► Output
```

---

## 1. Decisions to lock first

Settle these before any node code lands — cheap now, expensive later.

### 1.1 Control port convention

`docs/PLAN.md` describes `EPortType::Control` as "single float per block, smoothed." At 64 samples / 48 kHz that's 1.3 ms of resolution — coarse for ADSR attacks and audio-rate modulation.

**Decision:** promote Control to **audio-rate buffers**. Same `float[BlockSize]` shape as Audio; the distinction is semantic (link-type compatibility, default values on disconnect) not physical. No special-case buffer plumbing in `TNodeBase`.

Trade-off: ~2× RAM and per-sample CPU on modulation paths vs. one-float-per-block. Accepted.

### 1.2 Gate encoding

One float per sample. `> 0.5` = held. Note-on/off edge detection is the consumer's job — ADSR compares the previous sample's value to the current one to spot the transition.

### 1.3 MIDI ↔ audio thread crossing

RtMidi invokes its callback on its own platform thread (WinMM on Windows, CoreMIDI on macOS). That thread must not touch graph state directly.

**Decision:** one dedicated **SPSC ring buffer of `FMidiEvent { TimestampSamples, Status, Data1, Data2 }`**, written by the RtMidi callback, drained by `FMidiInput::Process` at the start of each block. This is **not** the general UI→Audio command queue from `docs/PLAN.md` §2 (still deferred — see `CLAUDE.md`).

### 1.4 Disconnected Control inputs

A `nullptr` input buffer on a Control port means "use this node's per-input default". Each node declares the default in `Prepare` (VCA gain = 1.0, gate = 0.0, filter cutoff = the param slider value, etc.). Avoids a single global rule that fits nothing.

### 1.5 Testing

Stand up **Catch2** as a second CMake target (`tests/`). Node DSP is pure-function-of-buffers — cheap to unit-test. Payoff grows through Phases 3–4. One smoke test per node at minimum: known input buffer → expected output.

---

## 2. Sub-phases

Each sub-phase is shippable on its own. 2A blocks everything; 2B / 2C / 2D are independent; 2E goes last.

### Phase 2A — Control foundations (~1 day, no audible change)

- Confirm `TNodeBase<NIn, NOut>` needs no shape change under the audio-rate Control decision (§1.1).
- Add `FOnePoleSmoother` utility. Apply it to `FOscillator::Amplitude` and `FGain::Gain` as reference implementations and to prove the pattern.
- Wire Catch2 into CMake. One smoke test per existing node.

**Done when:** existing Phase 1 patch still sounds identical; `cmake --build build --target tests` green on Windows + macOS CI.

### Phase 2B — Oscillator shapes (~1 day)

- `EOscShape { Sine, Saw, Square, Triangle, Noise }` + a Shape param on `FOscillator`. Use a combo box in the property panel, not a slider.
- **Saw, Square:** PolyBLEP to kill aliasing. Standard two-sample correction around each discontinuity.
- **Triangle:** leaky-integrated square (one-pole DC-blocker on the integrator).
- **Noise:** per-instance `xorshift32` PRNG; seed from node ID so graphs are deterministic on reload.

**Done when:** user can pick the shape per-oscillator and the waveforms are audibly distinct and non-aliasing.

### Phase 2C — VCA + ADSR + manual gate source (~2–3 days)

- **`FVca`** (`TNodeBase<2, 1>`, audio + control in, audio out). `Out = AudioIn * ControlIn`. Control disconnected → constant 1.0 (pass-through).
- **`FAdsr`** (`TNodeBase<1, 1>`, control gate in, control envelope out). Params: Attack / Decay / Sustain / Release (A, D, R in ms on log-scale sliders; S as 0–1 linear). State machine: `Idle → Attack → Decay → Sustain → Release → Idle`. Edge-trigger on gate; re-trigger during any state.
- **Temporary gate source** so 2C is testable before 2E lands. Either:
  - a throwaway `FGateButton` node (property panel holds a momentary toggle), or
  - an "Arm" toggle on a stub `FMidiInput` that just emits a gate without reading real MIDI.

**Done when:** `Osc → VCA → Output`, with `GateButton → ADSR → VCA.ctrl`, produces a plucked note on click.

### Phase 2D — State-variable filter (~2 days)

- **`FSvf`** (`TNodeBase<3, 3>`). Audio in, plus Cutoff and Resonance as **Control** inputs (also exposed as param sliders that supply defaults when disconnected). Three audio outs: LP, HP, BP.
- **Topology:** linear-trapezoidal (Vadim Zavalishin, TPT/ZDF form). Stable across the full cutoff × resonance range, ~20 lines. Skip Chamberlin — it blows up at high resonance / high cutoff and we'd just replace it.
- Clamp cutoff to `[20 Hz, 0.49 * SampleRate]` in `Process` to keep the bilinear transform well-conditioned.

**Done when:** audible cutoff sweep at resonance 0.8 is musical and stable; self-oscillates cleanly at resonance 1.0.

### Phase 2E — MIDI input (~2–3 days, biggest unknown)

- Add **RtMidi** to `CMakeLists.txt` via `FetchContent`. Upstream ships a CMake target we can link directly — no patching expected, but verify on both OSes before building on it.
- **`FMidiInput`** (`TNodeBase<0, 3>`, three Control outs: Gate, Frequency-in-Hz, Velocity 0–1). Monophonic last-note-wins allocator lives inside the node: maintain a small stack of held note numbers; on note-off, fall back to the previous held note if any.
- **Thread model:** RtMidi callback pushes `FMidiEvent` records into the SPSC ring from §1.3. `FMidiInput::Process` drains at block start, updates allocator state, fills the three output buffers (gate is edge-smoothed over 1 sample to avoid clicks; frequency is one-pole smoothed only in portamento mode if/when we add it — default off).
- **Property panel:** device-picker combo, channel filter (1–16 or Omni).

**Done when:** the target patch above plays from a MIDI keyboard on both Windows and macOS.

---

## 3. Sequencing & budget

```
2A (blocking) ──► 2B ──┐
                2C ────┼──► 2E (blocking on 2A; independent of B/C/D)
                2D ────┘
```

2B / 2C / 2D can go in parallel once 2A is in. 2E last — adds a third-party lib and a new thread, both easier to debug on top of nodes you already trust.

Rough total: **8–12 focused days**. Biggest risk is RtMidi cross-platform + the thread hop; everything else is textbook DSP.

---

## 4. Risks & mitigations

| Risk | Mitigation |
| --- | --- |
| Audio-rate Control burns more CPU than expected | Profile after 2D. If the mono patch already eats >5% of a modern core at 64-sample blocks, reconsider block-rate Control for non-audio modulation paths. |
| SVF instability at the edges (DC, Nyquist, resonance 1.0+) | Use the TPT form, not Chamberlin. Clamp cutoff. Add a Catch2 test that runs the filter at cutoff ∈ {20, 20 kHz}, resonance ∈ {0, 1}, checks output stays finite. |
| ADSR clicks on re-trigger | Start attack from the current envelope value, not 0. Documented as a standard ADSR quirk — don't reinvent. |
| RtMidi callback on a thread that also allocates when a device is hot-plugged | Device open/close happens on the UI thread, not the audio path. The callback itself only writes to the SPSC ring. If RtMidi turns out to allocate inside the callback, quarantine via an intermediate thread before the audio side. |
| MIDI jitter inside a block (event timestamp falls mid-block) | Phase 2: ignore — all events dequeued at block start act at sample 0 of the block. Sample-accurate MIDI is Phase 3+ concern. |
| Multiple `FOutput` nodes still silently ignored beyond the first | Carry-over from Phase 1. Either reject in `AddNode` when one already exists, or grey out the menu item. Small UX fix, bundle with 2A. |
| PolyBLEP / leaky-integrator triangle have subtle DC offsets | Catch2 test: generate N seconds of saw / square / triangle at several frequencies, assert mean < 1e-3. |

---

## 5. What stays deferred

- **Command queue (UI → Audio, general).** Still not needed. MIDI gets its *own* queue; do not generalize.
- **Polyphony.** Phase 3 only. `FMidiInput` is explicitly mono — do not design for voice allocation yet.
- **Patch save/load.** Phase 3. The `NodeFactory` string-ID registry that serialization needs should be introduced at the same time, not now.
- **Parameter smoothing on Control inputs.** Add smoothers *where they're audibly required* (MIDI frequency in portamento, slider-driven Amplitude/Gain) — not blanket-applied to every Control input. The plan's wording ("one-pole smoothing on every Control input, not just selected ones") is an anti-zipper measure; audio-rate Control from §1.1 already removes most of the zipper-noise motivation.
- **Layout persistence for the node editor.** Carry-over from Phase 1.
