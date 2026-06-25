# NodeSynth — Arpeggiator Plan

**Status: v1 shipped.** `INoteSink` refactor + `FArpeggiator` (`src/dsp/Arpeggiator.h`), palette registration, icon, `tests/Arpeggiator.test.cpp` (20 cases), and the bundled `Sequenced/Arp Pluck` preset are all in. The v2 items in §9 remain deferred.

A new `FArpeggiator` node (palette category **Control / sequencing**, sibling to `FClock` / `FSequencer` / `FGate`). It captures the set of currently-held notes and plays them back one at a time in a chosen order at a clocked rate, emitting the same `Gate` / `Frequency` / `Velocity` / `Note` Control outputs as `FVoiceAllocator` and `FSequencer`. That output-port compatibility is deliberate: the arp is a **drop-in monophonic replacement for the voice allocator** in the note → synth chain.

**Entry state:** Phases 0–5c + full effects roadmap + Wavetable + Mod Matrix + MIDI control complete. Note input is a transport concern handled by `FMidiDeviceManager` (audio thread) and the UI command ring (`FAudioCommandRing` → `FAudioGraph::DrainCommands`); both dispatch `HandleNoteOn` / `HandleNoteOff` to every `FVoiceAllocator` in the compiled snapshot. The on-screen keyboard and MIDI device both feed this path.

**Goal:** drop an Arpeggiator between note input and a synth voice, hold a chord, and hear it arpeggiated — Up / Down / UpDown / DownUp / AsPlayed / Random — synced either to the node's own internal BPM or to an external `Clock`, with octave range, gate length, swing, and latch.

**Final deliverable that defines "done":** `FArpeggiator` registered in the palette; a patch wired *note-input → Arp → Osc + ADSR → Gain → Output* produces an audible arpeggio from a held chord; all six patterns are audibly distinct; internal-clock and external-`Clock`-input modes both work; gate length, octaves, swing, and latch behave; the test suite covers held-note capture, pattern ordering, octave expansion, clock edge advance, gate windowing, and empty/mid-run mutation; zero allocation in `Process`.

---

## 1. The one architectural decision everything hangs on

**Decision: the arp is a note-event *sink* that emits stepped Control signals — exactly parallel to `FVoiceAllocator`, not a buffer-processing node.**

Notes are not Control buffers in this codebase. They arrive as discrete events delivered to allocators via `HandleNoteOn` / `HandleNoteOff` — on the audio thread from `FMidiDeviceManager::Process` (MIDI device + keyboard), and from `FAudioGraph::DrainCommands` (UI command ring). The allocator holds note state and *produces* the Gate/Freq/Vel/Note Control buffers that the rest of the graph consumes.

The arp does the same thing: it receives the same note events, maintains a **held-note set**, and on each clock step selects one note from that set per the active pattern, driving its four Control outputs. Because its output ports match the allocator's (`Gate`, `Frequency`, `Velocity`, `Note`), every downstream node that already works with the allocator — `ADSR`, `Oscillator`, etc. — works unchanged. A typical arp patch simply uses the arp *instead of* the allocator.

Rejected alternatives:
- **Arp reads the allocator's per-voice Gate/Freq buffers to discover held notes.** Loses note order, caps at 8 notes, couples two nodes through buffers that weren't designed for it. No.
- **Arp re-emits `NoteOn`/`NoteOff` to a downstream allocator** (so you get the allocator's voice management + overlapping release tails + true poly chord mode). Genuinely more powerful, but there is no node → node event channel today — only the manager and `DrainCommands` call `HandleNote*`, and they run *before* `FAudioGraph::Process`, so an arp emitting events from its own `Process` would land one block late. Deferred to v2 (§9).

**Consequence — held-note set is audio-thread-only.** `HandleNoteOn`/`HandleNoteOff` are only ever called on the audio thread (manager block-start dispatch, or `DrainCommands` at block start). So the held-note storage needs no atomics or locks — same as `FVoiceAllocator::Voices`. Params still go through atomics because slider drags push `SetParam` through the command ring.

### 1.1 Registering the arp as a note sink

The minimal, lowest-risk path mirrors the existing `MidiCcNodes` pattern: add a parallel `std::vector<FArpeggiator*>` everywhere `Allocators` appears.

**Recommended instead: introduce a small `INoteSink` interface** and have both `FVoiceAllocator` and `FArpeggiator` implement it. This future-proofs the note path (MPE, future note processors) and removes the duplicated dispatch loop.

```cpp
// dsp/NoteSink.h
class INoteSink
{
public:
    virtual ~INoteSink() = default;
    virtual void HandleNoteOn(uint8_t Note, float Velocity) = 0;
    virtual void HandleNoteOff(uint8_t Note) = 0;
};
```

Touch points (all replace `std::vector<FVoiceAllocator*> Allocators` with `std::vector<INoteSink*> NoteSinks`):
- `FAudioGraph` (`graph/Graph.h`) — the collected list.
- `FGraphModel::CompileFlattened` (`graph/Graph.cpp` ~line 1209) — the collection `dynamic_cast` becomes `dynamic_cast<INoteSink*>`, catching both node types in one branch.
- `FAudioGraph::DrainCommands` `NoteOn`/`NoteOff` cases — iterate `NoteSinks`.
- `FMidiDeviceManager::SetVoiceAllocators` → `SetNoteSinks`; its `Process` note-dispatch loop iterates `NoteSinks`.

`FVoiceAllocator` keeps its existing `HandleNoteOn`/`HandleNoteOff` signatures verbatim — it just gains the `INoteSink` base. Net change to the allocator: one base class, zero behaviour.

> Note: notes broadcast to *all* sinks. A patch containing both an arp and an allocator feeds the held chord to both simultaneously (poly chord + arpeggio at once). That is expected — arp patches normally omit the allocator. Document it in the node tooltip.

---

## 2. Node shape

`FArpeggiator : public TNodeBase<2, 4>, public INoteSink`

### 2.1 Ports

Inputs (both optional — `nullptr` when unconnected):

| Port | Type | Behaviour |
|------|------|-----------|
| `Clock` | Control | When **connected**, a rising edge (cross above 0.5) advances one step, overriding the internal clock — same override convention as `FOscillator`'s Freq input. When unconnected, the internal BPM/Rate clock runs. |
| `Reset` | Control | Rising edge jumps the step index back to 0. Mirrors `FSequencer`. |

Outputs (identical layout to `FVoiceAllocator` / `FSequencer` so downstream wiring is unchanged):

| Port | Type | Meaning |
|------|------|---------|
| `Gate` | Control | 1.0 during the gate-length window of the active step, else 0.0. |
| `Frequency` | Control | Hz of the active step's note. |
| `Velocity` | Control | Captured velocity of the active step's note (0..1). |
| `Note` | Control | MIDI note number of the active step (0..127). |

### 2.2 Params (`EParam`)

| Param | Kind | Range / choices | Default | Notes |
|-------|------|-----------------|---------|-------|
| `Pattern` | Choice | Up / Down / UpDown / DownUp / AsPlayed / Random | Up | Step ordering (§3). |
| `BPM` | Float | 20–300 | 120 | Internal-clock tempo; ignored when `Clock` input connected. |
| `Rate` | Choice | 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 | 1/16 | Steps per beat = {1, 2, 3, 4, 6, 8}; internal-clock only. |
| `Octaves` | Choice | 1 / 2 / 3 / 4 | 1 | Held set repeated, transposed +12 semis per extra octave (§3.2). |
| `Gate` | Float | 0–1 | 0.5 | Gate-high fraction of each step (reuses `FSequencer`'s gate-length idea). |
| `Swing` | Float | 0–0.75 | 0 | Delays every even step by `Swing × stepLength`. |
| `Latch` | Bool | — | false | When on, releasing all keys keeps the last set running until a *new* note (after silence) starts a fresh set (§4.3). |

`Glide` is intentionally omitted from v1 — portamento belongs on a downstream node or the future allocator coupling. `Clone()` returns `nullptr` (like `FVoiceAllocator`): the arp is a polyphony source, never a per-voice clone, so the per-voice flag is rejected.

---

## 3. Pattern resolution

The arp maintains the held set as two fixed arrays (no allocation), capped at `MaxHeld = 16`:
- `HeldNotes[MaxHeld]` with `{ uint8_t Note; float Velocity; uint32_t InsertionOrder; }`.
- A monotonically increasing `InsertionCounter` stamps insertion order for AsPlayed and as a stable tiebreak.

Each step, the **resolved sequence** is derived on the fly from the held set + pattern + octaves (no persistent expanded buffer needed; `StepIndex` indexes into the conceptual resolved sequence, and we compute the note for a given index). Define `N = held count`, `O = octaves`, `L = N × O` = resolved length.

### 3.1 Order within one octave

- **Up** — ascending by note number.
- **Down** — descending.
- **UpDown** — up then down, *endpoints not repeated*: `0,1,…,N-1,N-2,…,1` (period `2N-2`, falls back to `Up` for `N==1`). Avoids the double-hit at the turnaround.
- **DownUp** — the mirror.
- **AsPlayed** — insertion order.
- **Random** — uniform pick from the resolved set each step. Vary the per-step pick by `StepIndex` (no `Math.random` in the audio path — use a cheap LCG seeded in `Prepare`, advanced per step). Successive identical picks are allowed (classic arp behaviour); a "no immediate repeat" refinement is a v2 nicety.

### 3.2 Octave expansion

The single-octave order is repeated `O` times, each repeat transposed up by `12 × repeatIndex` semitones. So Up over `[60,64,67]` at 2 octaves yields `60,64,67,72,76,79`. UpDown applies the turn-around across the *full* expanded length.

### 3.3 Frequency

`freq = 440 · 2^((note − 69)/12)`, same helper as `FSequencer`/`FVoiceAllocator`. Transposed octave notes are clamped to ≤ 127.

---

## 4. Process algorithm

Per-sample loop, structured exactly like `FSequencer::Process` (edge detection + a sample-counted gate window), with the clock-source branch added.

```
for each sample I in block:
    detect Reset rising edge      -> StepIndex = 0; SamplesIntoStep = 0
    determine "advance" this sample:
        if Clock input connected:  advance = rising edge of Clock[I]
        else (internal):           advance = (SamplesIntoStep >= SamplesPerStep)
    if advance:
        StepIndex = (StepIndex + 1) mod max(L, 1)
        SamplesIntoStep = 0
        recompute SamplesPerStep   // from BPM+Rate (internal) or measured (external, like Sequencer)
        apply swing to this step's length if StepIndex is even
    // emit the active step
    if L == 0:                     // no notes held (and not latched)
        Gate[I]=0; Freq[I]=last;   // hold last freq to avoid a click in any tail
        Vel[I]=0; Note[I]=last
    else:
        resolve note for StepIndex (pattern + octaves)
        Freq[I] = NoteToFreq(note);  Vel[I] = vel;  Note[I] = note
        GateSamples = SamplesPerStep × Gate(param)
        Gate[I] = (SamplesIntoStep < GateSamples) ? 1 : 0
    ++SamplesIntoStep
```

- **Internal clock** uses a phase/sample accumulator like `FClock`: `SamplesPerStep = SampleRate × 60 / (BPM × stepsPerBeat)`.
- **External clock** measures the period between rising edges (copy `FSequencer`'s `SamplesPerStep = SamplesIntoStep` capture) so the gate window sizes itself to the incoming tempo.
- **Step retrigger is the downstream ADSR's job.** The arp emits a clean gate that drops to 0 for the tail of each step (when `Gate < 1`), then rises for the next. The downstream `FADSR` retrigger-from-current-level keeps fast arps click-free — identical to how the allocator + ADSR already behave. The arp itself does no smoothing.

### 4.1 Fresh-chord retrigger

When the held set transitions **empty → non-empty** (first key of a new chord, all previously released): reset `StepIndex = 0`, and in internal-clock mode reset `SamplesIntoStep = 0` so step 0 fires promptly rather than waiting up to a full step. This makes playing a chord feel immediate. (External clock still waits for the next incoming edge — the user asked to sync to that clock.)

### 4.2 Adding / removing notes mid-pattern

`HandleNoteOn` inserts into `HeldNotes` (ignore exact-note duplicates — refresh velocity instead, like the allocator's same-note path). `HandleNoteOff` removes and compacts the array. After either, `L` may change; clamp `StepIndex` into `[0, L)` with a modulo rather than resetting to 0, so the running pattern doesn't lurch. No reallocation — fixed arrays.

### 4.3 Latch

When `Latch` is on, `HandleNoteOff` does **not** remove the note from the *playing* set; instead it flags that all physical keys are up. The pattern keeps cycling the latched set. The next `HandleNoteOn` that arrives *while all keys are up* clears the latched set first and starts a fresh chord. This is the standard "arp hold" behaviour. Implementation: keep a `bool bAnyKeyDown` count and a `bLatchedSetSealed` flag.

---

## 5. UI

v1 uses the standard property panel — `Pattern`/`Rate`/`Octaves` combos, `BPM`/`Gate`/`Swing` sliders, `Latch` checkbox. No custom widget needed to ship.

**Nice-to-have (defer):** a small read-only strip in the property panel showing the resolved step sequence as note names with the active step highlighted, mirroring the `FSequencer` grid and `ScopeUI`/`MeterUI` custom-UI hooks in `Editor.cpp`'s `DrawPropertyPanel`. Reads `GetCurrentStep()` + the held set via test-style accessors.

---

## 6. Serialization

All params round-trip through the existing param-by-name patch JSON — no custom fields, no schema change. Held notes are runtime state and are *not* serialized (same as the allocator's live voices). Ship a bundled demo preset, e.g. `Sequenced/Arp Pluck`: note-input → Arp (Up, 1/16, 2 octaves) → Osc(Saw) + short-release ADSR → SVF → Gain → Output, so the empty-keyboard patch still makes sound once a key is held.

---

## 7. Integration checklist

1. `dsp/NoteSink.h` — new `INoteSink` interface.
2. `dsp/Arpeggiator.h` — the node (header-only, like `FClock`/`FSequencer`).
3. `FVoiceAllocator` — add `: public INoteSink` (no behaviour change).
4. `FAudioGraph` — `Allocators` → `NoteSinks` (type `std::vector<INoteSink*>`); update `DrainCommands`.
5. `FGraphModel::CompileFlattened` — collect `INoteSink*` (one `dynamic_cast`).
6. `FMidiDeviceManager` — `SetVoiceAllocators` → `SetNoteSinks`; dispatch loop over `NoteSinks`.
7. `ui/NodeRegistry.cpp` — register `"Arpeggiator"` for palette + factory + default-clone-by-name (though `Clone()` returns `nullptr`, the registry still needs the type-name → constructor entry for patch load).
8. Palette entry in `Editor.cpp` under **Control / sequencing** with the category colour/icon.
9. `tests/Arpeggiator.test.cpp` (CMake re-configure to pick up the new file).
10. Update `CLAUDE.md` node list/count and `docs/BACKLOG.md`.

`main.cpp`'s `PublishSnapshot` already hands the snapshot's note-sink list to the manager — the rename from `SetVoiceAllocators` to `SetNoteSinks` is the only change there.

---

## 8. Tests (`tests/Arpeggiator.test.cpp`)

Drive `HandleNoteOn`/`HandleNoteOff` directly + feed a synthetic Clock buffer (or run internal-clock for a measured number of samples), then assert on the output buffers — same harness style as the `FSequencer` and `FVoiceAllocator` tests. Set params *before* `Prepare` where step timing is asserted.

- **Held-note capture** — on/off updates the resolved length; duplicate on refreshes velocity, doesn't grow the set.
- **Up / Down ordering** — over a 3-note chord, successive steps emit ascending / descending frequencies.
- **UpDown turnaround** — endpoints not double-hit; period is `2N-2`.
- **AsPlayed** — order follows insertion, not pitch.
- **Octave expansion** — 2 octaves gives `2N` steps; octave notes are +12 semis and clamped ≤127.
- **External clock advance** — a rising edge on the `Clock` input advances exactly one step; internal clock ignored while connected.
- **Internal clock rate** — at 120 BPM / 1/16, step length ≈ `SampleRate × 60 / (120 × 4)` samples (range assertion, per the float-wraparound caveat in CLAUDE.md).
- **Gate window** — gate high for `Gate × SamplesPerStep`, low after.
- **Swing** — even steps delayed by the configured fraction.
- **Empty set** — `L==0` → gate stays low, no out-of-bounds.
- **Mid-run mutation** — adding/removing a note bounds `StepIndex` without OOB and without resetting to 0.
- **Latch** — pattern keeps running after all-notes-off; a new note after release starts a fresh set.
- **Random** — every emitted note is a member of the resolved set.
- **No allocation in `Process`** — hold an output pointer across many `Process` calls (matches the effect-node tests).

---

## 9. Deferred to v2

- **Drive a downstream `FVoiceAllocator`** (arp emits `NoteOn`/`NoteOff` events) for true polyphonic chord mode and overlapping release tails. Needs a node → node event channel and careful block-ordering vs the manager's pre-`Process` dispatch.
- **Chord mode** — strum/gate all held notes at once. Impossible on a mono `Frequency` port; arrives with the allocator-coupling above.
- **Converge / Diverge / random-walk patterns.**
- **Ratchet / step probability / per-step accent.**
- **"No immediate repeat" option for Random.**
- **Custom property-panel step display** (§5 nice-to-have).
- **Global transport sync** — share one tempo/PPQN clock across Clock, Sequencer, and Arp rather than per-node BPM.
