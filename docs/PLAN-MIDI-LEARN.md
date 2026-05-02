# NodeSynth — MIDI Learn (Hardware Controller Mapping)

A user-facing feature: right-click any node param's slider → **MIDI Learn** → wiggle a dial/slider on a hardware controller (e.g. Korg Nanokontrol2) → that physical control is now bound to that param. Subsequent CC messages on the bound control live-update the param, scaled to the param's range.

This plan covers PSID-style CC bindings: one physical control → one node param. Modulation-style routing (CC as a Control buffer feeding multiple targets through the graph) is a separate concern, deferred.

**Entry state:** Phase 5b complete (stereo signal path), SID Player feature done. `FMidiInput` node owns an `RtMidiIn` and an `FMidiRing` SPSC ring carrying note events to the audio thread. CC messages arrive at the same RtMidi callback but are currently dropped on the floor.

**Goal:** any user with a MIDI controller can map dials/sliders to node params in a few clicks, mappings persist with the patch, and the CC-driven updates feel as smooth as dragging the slider with the mouse.

**Final deliverable that defines "done":** with a Nanokontrol2 plugged in, the user can right-click `FOscillator.Frequency` → MIDI Learn → wiggle slider 1 → release → and from then on, slider 1 controls oscillator frequency. The mapping shows up as a small "MIDI" icon on the slider, persists across save/load, and can be removed via right-click → "Unmap MIDI". Nothing breaks if no MIDI device is connected.

---

## 1. Decisions to lock first

### 1.1 MIDI mapping scope: per-patch vs global

Two reasonable models, with the standard tradeoff:

- **Per-patch mappings** save with the JSON. Different patches can map the same physical control to different params. Matches what most synth plugins do.
- **Global mappings** ride with the user, stored in `~/.nodesynth/midi_mappings.json`. Reload-proof. Useful when the controller is set up once and the user just wants the same physical → logical mapping everywhere.

**Decision: per-patch for v1**, global as a deferred enhancement. Per-patch covers the workflow most users expect; global mappings can be added later as a "make this mapping default" toggle without breaking the per-patch primary path. Per-patch also makes save/load roundtrip the user's intent verbatim — no surprise behaviours when sharing patches.

### 1.2 Where the MIDI device lives

Today `FMidiInput` (the graph node) owns its own `RtMidiIn`. For MIDI Learn, we need CC events to flow whether or not the user has placed an `FMidiInput` node — except in practice, anyone using MIDI is going to have one anyway. Keeping device ownership on `FMidiInput` avoids a deeper refactor (no global manager singleton, no race between two consumers of the same device).

**Decision: extend `FMidiInput` rather than refactor to a global manager.** Add a second SPSC ring on `FMidiInput` for CC events; the existing note ring is unchanged. UI thread drains the CC ring per frame; audio thread continues to drain the note ring as today. If at some point we want MIDI Learn without a graph node, that's the trigger to do the global-manager refactor.

**Caveat to document in `docs/MIDI-LEARN.md`:** "to use MIDI Learn, place an `FMidiInput` node in the graph and select your device. Mappings work even if MIDI Input isn't otherwise wired into the audio path — the node just acts as the device owner."

### 1.3 CC value → param value mapping

CC values are 7-bit (0..127). Param target ranges vary:

- **Float param** — linear scale: `value = Min + (cc / 127) × (Max - Min)`. For `bLogarithmic = true` params, exponential: `value = Min × (Max/Min)^(cc/127)`.
- **Choice param** — integer index: `idx = floor(cc × NumChoices / 128)`, clamped to `[0, NumChoices-1]`.
- **Bool param** — threshold: `value = (cc >= 64) ? 1 : 0`.
- **String param** — not mappable. The right-click MIDI Learn menu item is hidden on string-kind widgets.

For v1 the mapping covers the param's full declared range. v2 can add per-mapping `MinOverride` / `MaxOverride` / curve customisation (so you can map slider 1 to "filter cutoff between 200 Hz and 8 kHz" instead of the param's full 20–20000 range).

### 1.4 Mapping data structure

```cpp
struct FMidiMapping
{
    uint8_t  Channel;       // 0..15 (or 16 for "any channel")
    uint8_t  Cc;            // 0..127
    FNodeId  NodeId;        // target node
    uint32_t ParamIndex;    // target param
};
```

Stored in a `std::vector<FMidiMapping>` owned by `FGraphModel` alongside Nodes / Links. Lookup: when a CC arrives, walk the vector to find matches (`Channel == anyOrSelf && Cc matches`). Linear scan is fine — typical patches have <30 mappings, and CC arrives at human-fingers rate (≤ a few Hz per fader normally, ≤100 Hz under aggressive sweep). Hash-map optimisation deferred.

### 1.5 Persistence (patch JSON)

Top-level `midi_mappings` array, additive (no schema bump):

```json
{
  "version": 1,
  "metadata": { ... },
  "nodes": [...],
  "links": [...],
  "midi_mappings": [
    { "channel": 0, "cc": 16, "node_id": 4, "param_index": 1 },
    { "channel": 0, "cc": 17, "node_id": 4, "param_index": 2 }
  ]
}
```

Old patches without the field load with an empty mapping list, no warnings. v1 doesn't roundtrip min/max/curve overrides because they don't exist yet — fields can be added later.

### 1.6 UI workflow

**MIDI Learn entry point:** right-click a param's slider widget → context menu adds **"MIDI Learn"** (top item) and **"Unmap MIDI"** (visible only if currently mapped). Selecting **MIDI Learn** puts the editor into "learn mode" for that specific (NodeId, ParamIndex). Visual feedback: the slider gets a pulsing yellow border; the bottom-of-screen status bar reads "Wiggle a control on your MIDI device, or press Esc to cancel." The next CC event captured establishes the mapping. Esc / Escape key / clicking elsewhere cancels.

**Visual indicator for mapped params:** mapped sliders display a small "M" badge (or a CC number, e.g. "CC16") in their right-most edge. Hovering shows a tooltip "Bound to channel 1, CC 16. Right-click to unmap."

**Conflict handling:** if the captured CC is already bound to a *different* param, replace the previous mapping silently. (User intent is "I want this physical control to do *this* now"; making them confirm every time is friction.) Optionally: brief toast message "Replaced mapping previously on Filter.Cutoff."

### 1.7 Threading

- **RtMidi callback** (its own thread, owned by `RtMidiIn`) — already running; classifies messages and writes to either `FMidiRing` (note events) or the new `FMidiCcRing` (CC events). Both rings are SPSC, so the callback is the sole producer.
- **UI thread** drains `FMidiCcRing` once per frame inside `Editor::Draw`. For each captured CC event:
  - If "learn mode" is active → finalise the new mapping, push it onto the model's mappings list (history-recorded so it can be undone).
  - Else → look up matching mappings, compute the target value per §1.3, dual-write: directly call `SetParamValue` on the target node (UI-side mirror) AND push a `SetParam` audio command (audio-thread side). Same pattern existing slider widgets use.
- **Audio thread** sees the resulting `SetParam` commands in the existing `FAudioCommandRing` queue. No new audio-thread paths.

This keeps everything synchronous from the UI's perspective and reuses the proven `SetParam` plumbing.

### 1.8 CC traffic rate and zipper

CC messages from a fader being aggressively swept can arrive at ~100 Hz. Each one bumps the param to a new value. For float params with smoothing already in place (gain, oscillator amplitude, LFO rate, virtual-keyboard mod, etc.), this is fine. For float params without smoothing, fast CC sweeps will produce zipper noise — same as fast slider drags do today.

**Decision: don't add per-mapping smoothing in v1.** If zipper appears in practice, the right fix is to add the same `FOnePoleSmoother` to the offending param at the node level (where it was needed for human slider drags too), not to special-case CC traffic.

### 1.9 Coalescing / undo

A CC sweep generates dozens of value changes. Each one is a SetParam. **Don't** push one history entry per CC event — the undo stack would fill in seconds. Mirror the slider-drag coalescing pattern:

- First CC on a mapping after a quiet period → capture "old value" snapshot.
- Subsequent CCs within (say) 500 ms of the last CC on the same mapping → just update the value, no history push.
- After 500 ms of CC silence on the mapping → push one history entry: SetParam (old → current).

Alternative (simpler): **don't track CC sweeps in the history at all.** They're an external input source, like MIDI notes — and we don't undo MIDI note events. The mapping itself is undoable; the parameter automation it produces isn't.

**Decision: don't track CC-driven param changes in history.** Matches the "external input source" mental model. Mapping creation/deletion still goes through history.

---

## 2. Sub-phases

```
MIDI.1 (capture CCs)         ──┐
MIDI.2 (mapping table)       ──┼──── ship as one feature
MIDI.3 (apply + UI)          ──┘
MIDI.4 (persistence)         ──── small follow-up
MIDI.5 (mappings panel)      ──── nice-to-have
```

### MIDI.1 — Capture CC events into a ring (~half day)

- New SPSC ring `FMidiCcRing` next to `FMidiRing` in `src/midi/`. Same template-style structure; payload is `{ uint8_t Channel, uint8_t Cc, uint8_t Value }`.
- `FMidiInput`'s RtMidi callback gains a CC branch: status nibble `0xB0` writes `(channel, cc, value)` to the new ring. Note events keep going to the existing ring.
- Public method `FMidiInput::DrainCcEvents(Callback)` for UI-thread consumption.

**Done when:** with a Nanokontrol2 plugged in, twiddling a fader produces CC events that an `FMidiCcRing::Pop` loop reads in real-time. Verified by a small dev hook printing `CC %d → %d` to stdout per CC drained.

### MIDI.2 — Mapping table on FGraphModel (~1 day)

- `FMidiMapping` struct in `src/graph/MidiMapping.h`.
- `std::vector<FMidiMapping>` member on `FGraphModel`. Accessors: `GetMidiMappings()`, `AddMidiMapping(Mapping)`, `RemoveMidiMapping(Mapping)`, `FindMappingForCc(Channel, Cc)`.
- `FEditCommand` variants `AddMidiMapping` / `RemoveMidiMapping`. Both undoable.
- `INode::Clone()` doesn't need to do anything (mappings live on the model, not the node).

**Done when:** mappings can be added/removed programmatically and survive Undo/Redo round-trips. Test fixtures in `tests/MidiMapping.test.cpp`.

### MIDI.3 — Apply CCs + UI for MIDI Learn (~1.5 days)

- In `Editor::Draw`, drain the FMidiInput's CC ring once per frame. Resolve target node, compute scaled value per §1.3, dual-write (`SetParamValue` + push `SetParam` audio command).
- Right-click context menu on every param widget gains **MIDI Learn** / **Unmap MIDI** items. Already-mapped params show a small "CC16"-style badge inline with the slider.
- "Learn mode" state on `FGraphEditorPanel`: `(LearningNodeId, LearningParamIndex)` — non-zero when learning. Captures the next CC event, creates the mapping, exits learn mode.
- Slider visual feedback for learn mode (pulsing yellow border) and mapped state (small inline badge).
- `Esc` cancels learn mode without binding.

**Done when:** the round-trip described in the "Final deliverable" section works end-to-end. With no MIDI device connected, the right-click menu still appears but learn-mode times out gracefully (no crash, just a "no MIDI device detected" toast).

### MIDI.4 — Patch JSON persistence (~half day)

- `PatchSerializer::SavePatch` emits `midi_mappings` array.
- `PatchSerializer::LoadPatch` reads it; absent field = empty list (back-compat with v1 patches).
- Tests: round-trip, ignore unknown fields gracefully.

**Done when:** save a patch with mappings, close the app, reopen — the mappings still control the same params.

### MIDI.5 — Mappings panel (nice-to-have, ~1 day)

A small docked panel listing all current mappings:

```
Channel  CC   Target                       [×]
───────────────────────────────────────────────
1        16   Filter (id 4) → Cutoff       [×]
1        17   Filter (id 4) → Resonance    [×]
1        18   Gain (id 5) → Gain           [×]
```

Click `×` to remove. Useful for orienting yourself in a complex patch and for fixing a misclick during MIDI Learn.

**Done when:** the panel exists, lists current mappings, and removing a mapping there has the same effect as right-click → Unmap MIDI on the bound param.

---

## 3. Sequencing & budget

```
Day 1:    MIDI.1 + MIDI.2 (CC ring + mapping table)
Day 2:    MIDI.3 (apply + UI)
Day 2.5:  MIDI.3 polish, edge cases
Day 3:    MIDI.4 (persistence) + MIDI.5 (panel)
```

Total: **~3–4 days**, with the panel slipping to a follow-up if time runs short.

---

## 4. Risks & mitigations

| Risk | Mitigation |
|---|---|
| User has no `FMidiInput` node — MIDI Learn does nothing. | Document the dependency. The right-click context menu shows "MIDI Learn" disabled with tooltip "Add a MIDI Input node first." Alternatively, surface a one-click "Add MIDI Input node" action from that tooltip. |
| Two `FMidiInput` nodes in one graph (rare but possible) — which one provides the CC stream? | Disallow at graph-validation time, with a clear error. Or arbitrate: first one in OrderedNodes wins. v1 picks the first; documented. |
| User remaps the same CC twice (silent override). | OK in §1.6. If it surprises users, add a brief toast ("Replaced mapping on Filter.Cutoff"). |
| Nanokontrol2 transport buttons (S/M/R, Play/Stop) emit CC too — they'd accidentally bind to params if mishit during MIDI Learn. | Acceptable v1 — the user can unmap and try again. v2 could filter "very brief CC events" (< some threshold) as button-hits and skip them. |
| Race between RtMidi callback writing CCs and UI-thread pop. | SPSC ring is lock-free; callback is sole producer, UI thread is sole consumer. Same pattern as the existing FMidiRing. No new race surface. |
| Mapping references a node that was deleted. | Sweep mappings on `FGraphModel::RemoveNode` — drop any mapping whose `NodeId` matches. Already a natural place to hook this. |
| String-kind params show MIDI Learn in their right-click menu (which doesn't make sense). | Skip the menu item for `EParamKind::String` widgets. `FSidPlayer.File` is the only such target today; would be wrong to bind a MIDI fader to a file path. |
| Choice / Bool params produce stuttering jumps under fast CC sweeps. | Expected behaviour — Choice 0..N maps to 128/N CC steps per choice. Document. |
| MIDI Learn fires on the *first* incoming CC, but some controllers send a snapshot of all current values when first opening the device. | Add a 200 ms guard window after entering learn mode where we ignore CC events, then capture the next one. Cheap insurance. |
| Multiple consumers of the same RtMidi device (FMidiInput's note path + CC path). | Both consume from the same callback, write to two different rings. Single-producer / single-consumer per ring; no contention. |

---

## 5. What stays deferred

- **Global mappings** (`~/.nodesynth/midi_mappings.json`) for "use the same physical → logical mapping across all my patches".
- **Per-mapping range overrides** — map CC 0..127 to a sub-range of the param. Useful for "make this fader do filter cutoff between 200 Hz and 8 kHz" instead of the param's full range.
- **Curve customisation** — log/exp/inverted/quantised curves per mapping. Currently we use the param's `bLogarithmic` flag and otherwise linear.
- **MIDI device selection in MIDI Learn UI** — for now, the MIDI Input node is the device owner; if the user wants to switch devices, they switch via the MIDI Input node's `Device` param.
- **Multi-CC mappings** — bind a single param to multiple CCs (e.g. coarse + fine). Niche; defer.
- **CC as Control source in the graph** — a dedicated node that exposes incoming CCs as Control output buffers, so they can drive any Control input via the standard wire-the-graph workflow. Different feature: complementary to MIDI Learn, not a replacement. Could be `FMidiCcSource` with N selectable CCs as Control outputs.
- **NRPN / RPN / 14-bit CC** — high-resolution controls. Most hardware uses 7-bit CCs; the rest is rare enough to defer.
- **MIDI input from VSTs / external software bridges** when running NodeSynth in plugin mode. Tied to the VST3 plan, deferred with it.
- **Take-over modes** (jump / pickup / scale) — when reloading a patch, the hardware control's physical position rarely matches the saved param value, causing a jump on first move. DAW conventions vary; v1 uses "jump" (next CC sets the value, ignoring physical position). v2 could add pickup mode.

---

## 6. Sources / references

- [Korg Nanokontrol2 manual](https://www.korg.com/us/products/computergear/nanokontrol2/) — default CC assignments per slider/knob, factory layout.
- Existing `src/midi/FMidiRing.{h,cpp}` and `src/dsp/MidiInput.{h,cpp}` — patterns to mirror for the CC ring.
- `docs/PLAN-PHASE-4.5.md` slider-drag coalescing — same pattern is reused for "treat CC sweeps as external input, don't pollute the undo stack".
