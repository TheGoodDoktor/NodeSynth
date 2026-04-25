# NodeSynth — Phase 3 Development Plan

Expands on Phase 3 of `docs/PLAN.md`. Entry state: Phases 0, 1, 2 complete plus the deferred SPSC `SetParam` queue (`src/graph/AudioCommand.h`). Subtractive mono synth target patch plays from MIDI input or the on-screen virtual keyboard. 39 Catch2 tests green.

**Phase 3 goal:** 8-voice polyphonic patches work and patches persist across sessions.

Final deliverable that defines "done": the user can build a polyphonic subtractive patch with an LFO modulating filter cutoff and a few math/utility nodes in the modulation path, save it to disk, quit the app, relaunch, load the patch, and play it polyphonically.

---

## 1. Decisions to lock first

Settle these before any code lands. Cheap now, painful to change later — particularly the save-file schema and the voice-allocator model.

### 1.1 JSON library

Save/load needs structured serialisation. Hand-rolling brittle.

**Decision:** **nlohmann/json** via `FetchContent`. Header-only, ergonomic, widely used. ~1 MB header but only the parts we use compile. RapidJSON is faster but more boilerplate; we don't have a perf concern at this scale.

### 1.2 Save file schema

Lock the shape now so versioning is sane.

```json
{
  "version": 1,
  "metadata": { "name": "Untitled", "created": "2026-04-25T10:00:00Z" },
  "nodes": [
    { "id": 1, "type": "Oscillator", "x": 60.0, "y": 120.0,
      "params": { "Shape": 1, "Frequency": 440.0, "Amplitude": 0.5 } }
  ],
  "links": [
    { "id": 1, "from_node": 1, "from_port": 0, "to_node": 2, "to_port": 0 }
  ]
}
```

- `version` is an integer. Bump on breaking schema changes; refuse to load mismatched versions in v0.
- `type` is `INode::GetTypeName()` — same string the registry keys on.
- `params` is a name → value map (uses param info names from `GetParamInfos`, not indices, so reordering params doesn't break old files).
- Node IDs preserved verbatim. `FGraphModel` accepts an explicit ID on load (one new overload of `AddNode`).
- Transient state (e.g. `FMidiInput::DeviceNames`, `FVirtualKeyboard` held-note stack) does **not** roundtrip.

### 1.3 Math / utility node port shapes

| Node | Inputs (Control) | Outputs (Control) | Params |
|---|---|---|---|
| `FAdd` | A, B | Out = A + B | – |
| `FMultiply` | A, B | Out = A * B | – |
| `FScale` | In | Out = OutMin + (In - InMin) * (OutMax - OutMin) / (InMax - InMin) | InMin, InMax, OutMin, OutMax |
| `FConstant` | – | Out = Value | Value |
| `FSampleHold` | In, Trigger | Out (latched In on trigger rising edge) | – |

Disconnected inputs read as 0 for `FAdd`, 1 for `FMultiply` (so they act as identity), 0 for `FScale.In`, 0 for `FSampleHold.In`, 0 for `FSampleHold.Trigger`.

### 1.4 LFO node

```
FLfo  inputs : Sync (Control, optional — phase reset on rising edge)
      outputs: Out (Control, bipolar [-1, 1])
      params : Shape (Sine/Triangle/Saw/Square), Rate (Hz, log slider 0.01..50), Amount (0..1)
```

Free-running by default. Bipolar output — users scale with `FScale` or `FMultiply` for unipolar. No per-voice phase reset until polyphony is in (then `Sync` from the voice allocator's gate retriggers per voice).

### 1.5 Voice allocator model

The hardest decision. Three approaches considered:

**(a) Polyphonic cables** (VCG Rack model). Cables carry N voices; every node processes N times internally. Cleanest user mental model. Massive engineering: every node's `Process` becomes voice-aware. Out of scope for Phase 3.

**(b) Per-voice subgraph clone at compile time.** User marks part of the graph as "per-voice"; the compiler clones reachable downstream nodes N times and wires them up. Conceptually clean but requires the compiler to deep-clone nodes (every node gains a `Clone()` virtual) and wires for N voices.

**(c) Single `FVoiceAllocator` node containing N voice instances internally.** The voice instances are *fixed*: a hardcoded mini-patch (Osc → SVF → VCA + ADSR) built into the node. The node's params expose the inner Osc/SVF/ADSR's params. Easy to implement. Bad: users can't shape voices with the node graph.

**Decision:** approach **(b), per-voice clone**. Make `INode::Clone()` a virtual that returns a new instance with the same param values. `FGraphModel` gains a `bool bPerVoice` flag per node (set by the user via context menu). At compile time, reachable per-voice nodes get cloned `NumVoices` times; non-per-voice nodes remain singletons. The compiled graph has one set of "global" nodes (e.g. Output, master Gain) and N parallel sets of voice nodes summed at the boundary.

Voice allocation is owned by a special `FVoiceAllocatorSource` node that replaces `FMidiInput` / `FVirtualKeyboard` as the source. It exposes `Gate[i]`, `Frequency[i]`, `Velocity[i]` ports — but those are *per-voice* ports that the compiler matches to clone index `i`. Note assignment uses the existing held-note-stack code with voice-stealing (oldest released first, then oldest held).

### 1.6 NoteOn / NoteOff commands

Polyphony needs note events flowing to specific voices, not whole-graph param edits.

```cpp
enum class EAudioCommand : uint8_t
{
    SetParam,
    NoteOn,    // Data1 = note number, Data2 = velocity 0..127
    NoteOff,   // Data1 = note number
};
```

Pushed by the input nodes (MIDI Input, Virtual Keyboard) and routed through `FAudioGraph::DrainCommands`. The voice allocator node consumes them and updates per-voice atomics.

### 1.7 Voice count

8 voices fixed for v1. User-configurable count is a `FVoiceAllocator` param but requires a graph recompile when changed (per-voice clones are baked at compile time).

### 1.8 Editor layout persistence

Settle alongside save/load since they touch overlapping concerns.

**Decision:** ImGui `imgui.ini` + imgui-node-editor's per-editor settings file both written to a `~/.nodesynth/` (or platform equivalent) directory. *Not* baked into the patch file — layout is user/window state, not patch state.

---

## 2. Sub-phases

```
3A (math/util)   ──► 3B (LFO) ──┐
3C (save/load)   ────────────────┼──► 3E (voice impl)
3D (voice design) ───────────────┘
```

3A and 3B are independent warmups. 3C and 3D run in parallel — 3D is design-only, 3C is implementation. 3E is the largest single chunk and depends on the voice design being locked.

### Phase 3A — Math & utility nodes (~1 day)

- Five new node classes in `src/dsp/`: `FAdd`, `FMultiply`, `FScale`, `FConstant`, `FSampleHold`. All header-only, all `TNodeBase<N, 1>`. Match the port shapes from §1.3.
- Add each to `src/ui/NodeRegistry.cpp` with a tooltip description.
- Add an icon for each in `src/ui/NodeIcons.cpp`. `FAdd`/`FMultiply` are `+` / `×` glyph shapes, `FScale` is two arrows changing slope, `FConstant` is a horizontal line, `FSampleHold` is stair-step.
- Catch2 tests per node (basic input/output, disconnected-input defaults, S&H trigger edge detection).

**Done when:** the nodes are usable in the graph editor and their tests pass.

### Phase 3B — LFO node (~half day)

- `FLfo` (`TNodeBase<1, 1>`) with the spec from §1.4.
- Phase accumulator state. Sine via `std::sin`; triangle / saw / square computed from phase directly (no PolyBLEP needed — modulation rates are well below audio).
- Sync input: rising-edge detection resets phase to 0.
- Smoothed Rate to avoid zipper.
- Catch2 tests: each shape produces expected zero-crossings; sync resets phase; rate is correct in Hz at 48 kHz.

**Done when:** LFO modulating an oscillator's amplitude or filter cutoff is audibly correct.

### Phase 3C — Patch save/load (~3-4 days)

- Add **nlohmann/json** to `CMakeLists.txt` via `FetchContent`.
- `src/io/PatchSerializer.{h,cpp}`: `void SavePatch(const FGraphModel&, const std::filesystem::path&)` and `std::optional<FLoadResult> LoadPatch(const std::filesystem::path&)`. `FLoadResult` carries the new model + a vector of `SetParam` commands to push for the freshly compiled graph.
- Each node exposes `GetParamInfos()` already; serialiser walks it to build `params`. Loader looks up params by name (not index).
- `FGraphModel::AddNodeWithId(...)` overload for restoring exact IDs.
- Loader replays params through the audio command queue once the snapshot is published, so post-load state is queue-aligned with subsequent edits.
- File menu in the main window: New / Open / Save / Save As. Native file dialogs via [`nfd`](https://github.com/btzy/nativefiledialog-extended) or similar (small dependency, cross-platform).
- **Singleton `FOutput` enforcement** lands here: `Compile` already silently picks the first; loader explicitly drops extras with a warning. Also reject `AddNode` on a second `FOutput`.
- Editor layout persistence (§1.8) bundled here.
- Catch2 tests: round-trip a synthetic graph, assert all params and links match.

**Done when:** save → quit → relaunch → load reproduces the patch, including transitive state like the keyboard's mod-wheel position. Two-output rejection is tested. Layout persists across runs.

### Phase 3D — Voice allocator design (~2 days, design-only)

Lock approach (b) from §1.5 in detail before implementing.

- Specify `INode::Clone()` semantics. Atomic state (param atomics) clones with current values; transient state (oscillator phase, ADSR stage, filter state, smoother) resets via `Prepare`. RtMidi-owning nodes (`FMidiInput`) and per-instance UI state (`FVirtualKeyboard` held notes) are *not cloneable* — flagged at compile time.
- Specify the per-voice flag UI: right-click context menu on a node → "Mark as per-voice." Visual indication (border colour or icon overlay) on the node header.
- Specify `Compile()` changes:
  1. Walk reachable nodes from each `FOutput`.
  2. Partition into per-voice and global sets.
  3. Validate: per-voice → global edges OK (voice contributions sum into global Audio inputs); global → per-voice edges OK (e.g. a global LFO modulating per-voice filter cutoff broadcasts to all voices); per-voice → per-voice edges *within the same voice* OK; per-voice → per-voice across voices is an error.
  4. For per-voice nodes, instantiate `NumVoices` clones, plumbed in parallel.
  5. Voice-allocator source emits `NumVoices` parallel sets of Gate / Frequency / Velocity buffers; the compiler matches them to voice indices.
- Voice stealing policy: oldest released voice first; then oldest held. Document explicitly.
- Specify graph-snapshot lifetime: cloned nodes are owned by the compiled snapshot only — not added to `FGraphModel::Nodes`. UI shows the user-edited graph; audio runs the cloned one.

**Done when:** a written design in this file (or a follow-on `PLAN-PHASE-3-VOICES.md`) covers every bullet above with locked answers.

### Phase 3E — Voice allocator implementation (~5-7 days)

- Add `Clone()` to `INode` and override on every cloneable node. Reset transient state in `Prepare`.
- New `FVoiceAllocator` source node replacing `FMidiInput` / `FVirtualKeyboard` as the front end (existing nodes stay for backwards compat / mono patches; the voice allocator wraps them or replaces them).
- `EAudioCommand::NoteOn` / `NoteOff` variants + `FAudioGraph::DrainCommands` dispatch.
- `FGraphModel`: per-node `bPerVoice` flag, validation in `AddLink`, partitioning + cloning in `Compile`.
- Voice stealing inside `FVoiceAllocator`: track per-voice (note, age, gate) tuples; on a new note find the oldest released or oldest held.
- Catch2 tests: 8 simultaneous note-ons reach 8 distinct voices; 9th note steals the oldest released first; per-voice ADSR releases independently; cross-voice edge rejected at `AddLink`.
- Update the seeded default patch to demonstrate polyphony (still one voice in the actual layout, but with the per-voice flag set so multiple notes work).

**Done when:** holding 8 simultaneous notes on the virtual keyboard produces 8 audible voices, releasing one drops only that voice's envelope, and a 9th note cleanly steals.

---

## 3. Sequencing & budget

```
Day 1:        3A
Day 2:        3B + start 3D design
Day 3-6:      3C (with 3D continuing in parallel)
Day 7-8:      3D wraps; 3E begins
Day 9-15:     3E
```

Rough total: **12-15 focused days**. Variance dominated by 3E — voice allocation has the most unknowns, particularly around the partitioning step in `Compile`.

Save 3D headroom: don't let voice design slip past Day 8. If the design isn't locked by then, ship a stripped Phase 3 with monosynth save/load (3A + 3B + 3C only) and split voices into Phase 3.5.

---

## 4. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Voice allocator design lands wrong (e.g. realises late that approach (b) doesn't compose with global modulation sources) | 3D produces a *written, complete* design before any 3E code. If a hole shows up during 3D, revisit (a) or (c). |
| Cloning shared resources (RtMidi, audio device handles) blows up | Mark non-cloneable node types; `Clone` is `nullptr`-able. `FGraphModel::Compile` rejects per-voice flags on non-cloneable nodes with a clear error. |
| 8 voices × full DSP eats too much CPU | Profile after a 3E milestone with all 8 voices firing. Drop to 4 voices as a temporary fallback if needed; cite the bottleneck before optimising. |
| JSON schema reordering breaks old files | Params keyed by *name* not index. Versioned format. Refuse to load mismatched major versions. |
| Save file leaks transient state (RtMidi device handle, pressed virtual keys) | Explicit allow-list per node: only `GetParamInfos()` params roundtrip. Anything else is recomputed on load. |
| Voice stealing audibly clicks | Stolen voice's amp envelope finishes its release into the new note's attack — re-trigger preserves current level (already ADSR's behaviour). |
| Two-output graphs survived from before §3C — opening one crashes or sounds broken | `Compile` already keeps the first; loader logs and drops extras. `AddNode` rejects a second `FOutput` outright once 3C lands. |
| Layout persistence dir behaves badly across OSes (Windows env vars, macOS sandbox) | Use `std::filesystem::path` with platform-detected home dir. Fall back to next-to-binary if HOME is unset. Smoke-test on both Windows and macOS CI. |
| Math/util nodes land first and the graph fills up with `FConstant` chains the moment users start patching | Cosmetic. If it gets unwieldy, a `FConstant` could be inlined into other nodes' param sliders later. Not a Phase 3 concern. |

---

## 5. What stays deferred

- **Structural commands through the queue** (`AddNode` / `RemoveNode` / `Connect` / `Disconnect`). Snapshot-swap remains. RT-safe structural mutation is its own milestone — see `CLAUDE.md`.
- **Sample-accurate MIDI**. Events still quantise to sample 0 of the drain block. Phase 4+.
- **Undo / redo**. Phase 4 deliverable per `docs/PLAN.md`.
- **Polyphonic-cable model.** Approach (a) from §1.5 — cleaner UX but 3-4× the engineering. Revisit only if approach (b) proves to have user-facing limitations.
- **MIDI in / out beyond keyboard input.** No CC, no pitch-bend in `FMidiInput`. Add when a patch needs them.
- **MPE.** Out of scope.
- **Hot-swapping voice count without recompile.** `NumVoices` triggers a graph recompile by design. Live re-voicing is a Phase 4+ optimisation.
- **Per-voice modulation routing UI**. The per-voice flag is a node-level toggle, not a connection-level one. If users want different mod routing per voice they get a Phase 4 redesign.
