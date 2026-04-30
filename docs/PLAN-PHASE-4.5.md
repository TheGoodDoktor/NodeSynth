# NodeSynth — Phase 4.5 Tidy-up Plan

A focused interlude between Phase 4 (depth) and Phase 5 (stereo / oversampling / presets / VST3). Picks up the small + medium loose ends accumulated during Phases 1–4 without taking on architectural risk. Nothing here blocks Phase 5; treat the whole thing as scope-flexible polish.

**Entry state:** Phases 0–4 complete. 168 Catch2 tests / 64k+ assertions green. Compile-error UX (last-good snapshot stays live + red-link marker + banner) is in. `FMixer` for layered patches is in.

**Phase 4.5 goal:** the synth feels finished — node editor has the keyboard / mouse interactions users expect, save/load roundtrips patch metadata, undo covers position changes, polyphony picks up portamento and per-voice LFO sync. No DSP rewrites, no new architectural commitments.

**Final deliverable that defines "done":** all of 4.5a + 4.5b + 4.5c land cleanly with tests. The deferred list at the end is *intentionally* deferred — it stays for Phase 5 or later, not "we ran out of time".

---

## 1. Decisions to lock first

### 1.1 Native file dialog library

**Decision:** [`nfd-extended`](https://github.com/btzy/nativefiledialog-extended) via `FetchContent`. Cross-platform, small (~400 KB compiled), CMake-friendly. Used by enough open-source ImGui apps that drop-in failures are rare.

The existing typed-path popup stays as a fallback path for headless / sandboxed environments where the OS file dialog API is unavailable.

### 1.2 Patch metadata schema

Add a top-level `metadata` object to the patch JSON:

```json
{
  "version": 1,
  "metadata": {
    "name": "Soft Pad",
    "author": "Mark",
    "bpm": 120.0,
    "notes": "All voices through reverb; tweak room size for room ambience.",
    "created_at": "2026-04-30T10:00:00Z",
    "sample_rate_hint": 48000
  },
  "nodes": [...],
  "links": [...]
}
```

All fields optional. Loader treats absent fields as defaults, no schema bump needed (additive change). UI surfaces them in a small "Patch Info" panel.

### 1.3 Position undo/redo

`imgui-node-editor` owns runtime positions; `FNodeRecord::PositionX/Y` is currently just a seed. **Decision:** sync editor → model once per frame at end of `FGraphEditorPanel::Draw`, *only* for nodes whose position changed since last frame. When a change is detected and history recording is enabled, push a `SetNodePosition` edit-history entry (new `EEditCommand` variant).

Position-change detection: cache the last-frame position per node, compare each frame. Cheap (one `unordered_map<FNodeId, ImVec2>` lookup per node).

Coalescing: like slider drags, only commit a history entry when the user *finishes* a drag — `ed::IsNodeSelected(...)` plus a transition watcher. Realistic implementation: capture position-on-drag-start, push history-on-drag-end. Falls back to "every frame's position change pushes one entry" if the editor's drag-state API isn't usable.

### 1.4 Portamento (Glide)

`FVoiceAllocator::Param_Glide` already exists (0..2000 ms, default 0). **Decision:** smooth the per-voice Frequency output via a `FOnePoleSmoother` per voice, time-constant = Glide ms. NoteOn snaps to the new note (no glide on retrigger) when Glide=0; non-zero Glide slides from the previous note. No mono-vs-poly mode toggle for v1 — glide just always applies to whatever the previous voice frequency was.

### 1.5 Per-voice LFO Sync

LFO is cloned per-voice already (it's just a node like any other). The Sync input wires per-voice → per-voice and resets the *clone's* phase, not a global. So this should already work — flag this as **needs verification, not implementation**. If it doesn't, the fix is small: confirm the LFO's Sync input is treated as Control + per-voice clone receives voice-i's sync edge.

### 1.6 Sample-accurate MIDI

Add a `uint16_t SampleOffset` field to `FAudioCommand` (re-pack to keep struct alignment). Note events: `SampleOffset` = sample within the next-to-process block. UI-thread sources push with offset 0 (they don't know the audio thread's position); RtMidi callback timestamp can be converted to a sample offset when `FMidiInput` enqueues.

`FAudioGraph::DrainCommands` becomes more involved: instead of draining everything at block start, it drains *up to* the current sample index. Process-loop becomes:

```
for each block:
    for each event in queue with offset < block_size:
        dispatch at SampleOffset
        increment "events processed up to" cursor
    process samples
```

Trade-off: more complex Process. Audible improvement at 64-sample blocks is ~1.3 ms quantisation → ~0 ms. Marginal. Defensible to skip.

### 1.7 Multi-select model

`imgui-node-editor` already supports multi-select (Ctrl+click, marquee). The model layer needs to handle batch operations: `RemoveSelectedNodes`, `DuplicateSelectedNodes`, etc. Each batch operation pushes a single coalesced history entry (a new `EEditCommand::Composite` variant carrying a vector of sub-commands), so a multi-delete is one undoable event.

---

## 2. Sub-phases

```
4.5a (Editor UX)        ──┐
4.5b (Save/load polish) ──┼──── ship
4.5c (DSP polish)       ──┘
```

Three sub-phases, mostly independent. Pick by appetite; 4.5a has the highest visible-polish payoff.

### Phase 4.5a — Editor UX (~2-3 days)

- **Node context menu** — extend the existing right-click popup (currently has the "Per-voice" toggle) with **Delete**, **Duplicate**, plus a separator. Duplicate clones the node via `INode::Clone()` and offsets the new node's position by `(40, 40)` so it's visible. Each menu item pushes one history entry.
- **Multi-select batch ops** — `RemoveSelectedNodes`, `DuplicateSelectedNodes`. Both wrap their work in `Composite` history entries. `imgui-node-editor`'s `ed::GetSelectedNodes(buffer, max)` retrieves the selection.
- **Node search** in the palette — text input above the list filters by `MenuLabel` substring match (case-insensitive). Empty filter shows all (current behaviour).
- **Native file dialog** — `nfd-extended` via `FetchContent`. Replace the typed-path popup body with the dialog. Keep the popup as a fallback when the dialog API returns an error.
- **Master meter on the seeded patch** — one-line addition to `SeedDefaultPatch`: insert an `FMeter` between `Gain` and `Output`. New users immediately see signal levels.

**Done when:** right-click on a node shows Delete / Duplicate / Per-voice; selecting multiple nodes and pressing Delete removes them all in a single undoable action; typing in the palette filter narrows the list; Open / Save As pop a real OS file dialog; the seeded patch shows level metering on the master bus.

### Phase 4.5b — Save / load + history polish (~2 days)

- **Patch metadata** — `metadata` object in `Save/LoadPatch` (§1.2). New `FPatchMetadata` struct on the loaded result; `FGraphModel` doesn't need to store it (it's UI-display state). Property-panel-style "Patch Info" panel with editable Name / Author / BPM / Notes fields.
- **Sample-rate mismatch warning** — patch records `sample_rate_hint`; on load, if it differs from the device sample rate, show a one-line yellow warning in the load result toast. Doesn't block the load.
- **Position undo/redo** — `EEditCommand::SetNodePosition` variant. `FGraphEditorPanel::Draw` caches last-frame positions, detects drag-end via `ed::IsNodeSelected` transition or simpler "position stopped changing for one frame", pushes a coalesced history entry per drag.
- **Edit history visual panel** — small docked panel listing the last N undo entries with their type names and brief context (e.g. "Add Oscillator", "SetParam Cutoff"). Click an entry to jump there (calls `Undo` or `Redo` repeatedly until at the chosen index). Display-only — not load-bearing.

**Done when:** patches roundtrip name / author / BPM / notes; loading a 96 kHz patch on a 48 kHz device flags a warning; dragging a node and releasing is one undo step; the History panel shows recent actions.

### Phase 4.5c — Polyphony + DSP polish (~3-4 days)

- **Portamento** — wire `FVoiceAllocator::Param_Glide` to a per-voice `FOnePoleSmoother` on the Frequency output. Per §1.4: smoother time constant tracks the param value; NoteOn writes the target frequency, the smoother slides to it.
- **Per-voice LFO Sync verification** — confirm the existing LFO clones receive per-voice gate edges via the partition algorithm. If broken: small fix to wire correctly. If already working: just write the test.
- **Proactive link rejection on drag** — factor the per-voice → mono Control validation out of `Compile` into a free `IsLinkValid(FromNode, FromPort, ToNode, ToPort, Model)` helper. Call it inside `ed::BeginCreate` / `ed::AcceptNewItem`, reject with a red tooltip explaining why. Current banner + red-link survives as a backup for paths that don't go through the drag flow.
- **Sample-accurate MIDI** (optional, may skip) — per §1.6. Significant Process-loop change; audible benefit at 64-sample blocks is marginal. Include only if time allows.

**Done when:** glide of 200 ms produces an audible pitch slide between consecutive notes; LFO Sync from a per-voice gate retriggers each voice's LFO independently; dragging a per-voice → mono Control link is rejected before the link is created.

---

## 3. Sequencing & budget

```
Day 1-2:   4.5a (Editor UX)
Day 3-4:   4.5b (Save/load + history)
Day 5-7:   4.5c (Polyphony + DSP polish)
```

Total: **~7 focused days**, with sample-accurate MIDI possibly bumping it to 9-10 if included.

If you only do 4.5a, ship as Phase 4.5 and defer the rest to 4.6 — 4.5a is by far the highest user-visible value.

---

## 4. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Position-change detection per-frame is noisy (small float drift triggers entries) | Quantise positions to integer pixels before comparison; require ≥1 pixel change. |
| Multi-select Composite history entries get nested awkwardly when a single user action triggers multiple sub-commands (e.g. RemoveNode → also remove incident links) | RemoveNode already captures incident links in the existing history entry. Composite is only used for batch ops where the user explicitly selected multiple nodes. |
| Node-search filter races with palette drag-drop | Filter is purely visual — drag sources still emit by registry index. Test: with a filter active, dragging the visible row drops the right node type. |
| `nfd-extended` build failures on macOS 10.15 / Linux without GTK | Document the typed-path fallback. CI matrix should test both code paths. |
| Glide produces sluggish first note if the previous voice's freq is far away | Common synth behaviour — a hard "first note" mode would need either a `bFirstNote` per-voice latch or a separate "Mono Glide" mode. Punt to a follow-up if it bothers anyone. |
| Sample-accurate MIDI struct change breaks the existing SPSC queue layout | `FAudioCommand` is fixed-size; adding a `uint16_t` field needs corresponding fixed-size adjustment. Tests cover the queue's binary layout (push/pop FIFO order); ensure they still pass after the field add. |
| Edit history panel scope-creeps into a full transaction-log view | Display-only listing with click-to-jump. No editing of past entries, no branching alternate timelines. Document this and stop. |

---

## 5. What stays deferred

These remain on the parking lot — none belongs in Phase 4.5:

- **Structural commands through the SPSC queue.** `AddNode` / `RemoveNode` / `Connect` / `Disconnect` still snapshot-swap. Big architectural lift; no Phase-3/4 deliverable required it.
- **Live re-voicing without recompile.** Changing `NumVoices` still triggers a recompile.
- **MPE** — per-note pitch / pressure. New command shape, fundamentally bigger than this tidy-up's scope.
- **Convolution / plate / hall reverb.** Freeverb covers v1; richer reverbs are Phase 5+.
- **Tap-tempo / time-stretch on `FDelay`.**
- **External clock sync** — MIDI clock, Ableton Link, ReWire. Each is its own meaty integration.
- **Polyphonic cables** — alt voice architecture, decided against in `PLAN-PHASE-3-VOICES.md`.
- **Multi-allocator UI affordance.** Broadcast works fine when there's only one allocator, which is the current reality. Defer until someone wants two.
- **Stereo signal path** — Phase 5.
- **Oversampling for nonlinear nodes** — Phase 5.
- **Presets browser** — Phase 5.
- **VST3 wrapper** — Phase 5+.

If user feedback flips any of these from "deferred" to "blocking", revisit then — not now.
