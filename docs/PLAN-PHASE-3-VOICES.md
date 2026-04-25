# NodeSynth — Phase 3D: Voice Allocator Design

This is the locked design that Phase 3E will implement. Expands `PLAN-PHASE-3.md` §1.5 ("Voice allocator model") and §2 Phase 3D into something concrete.

**Goal:** 8 simultaneous notes work on a polyphonic patch. Releasing one drops only that voice's envelope. A 9th note steals cleanly with no click.

**Entry state:** Phases 0–2 + Phase 3A (math/util) + 3B (LFO) + 3C (save/load) all in. SPSC queue carries `SetParam`. 65 tests green.

**Final deliverable:** the seeded patch (`VirtualKbd → Osc + ADSR → Out`) marked per-voice from Osc through ADSR plays 8 simultaneous notes when 8 keys are held.

---

## 1. Decisions to lock

### 1.1 Approach (locked from `PLAN-PHASE-3.md` §1.5)

**Per-voice subgraph clone at compile time.** The user marks individual nodes as "per-voice"; `Compile()` instantiates `NumVoices` clones of each per-voice node and wires them in parallel. Voice allocation happens inside a dedicated `FVoiceAllocator` node that drains `NoteOn` / `NoteOff` commands from the audio command queue.

Rejected alternatives (and why):
- **Polyphonic cables** (VCV Rack model): cleanest UX, but every node would need to be voice-aware. 3–4× the engineering for Phase 3.
- **Single monolithic poly-synth node** with hardcoded voices: cheap, but users can't shape voices with the node graph — defeats the point of a node synth.

### 1.2 Voice count

**Fixed at 8 for v1.** Exposed as a `Param_NumVoices` Choice on `FVoiceAllocator` so save/load roundtrips, but changing it triggers a graph recompile (cloning is baked at compile time). Live re-voicing without recompile is a Phase 4+ optimisation.

### 1.3 `INode::Clone()` semantics

New virtual on `INode`:

```cpp
virtual std::shared_ptr<INode> Clone() const;
```

**Default implementation** (provided in `INode` itself, leveraging the existing factory + param-name copy):

```cpp
std::shared_ptr<INode> INode::Clone() const
{
    auto Cloned = MakeNodeByTypeName(GetTypeName());
    if (!Cloned) return nullptr;  // unknown type — non-cloneable
    const auto Infos = GetParamInfos();
    for (uint32_t I = 0; I < Infos.size(); ++I)
    {
        const int32_t TargetIdx = FindParamIndex(*Cloned, Infos[I].Name);
        if (TargetIdx >= 0) Cloned->SetParamValue(TargetIdx, GetParamValue(I));
    }
    return Cloned;
}
```

This mirrors `LoadPatch`'s flow exactly (instantiate by type name, copy params by name) so the code already exists and is tested.

**State semantics:**
- **Param state** (`std::atomic<float>` members exposed via `GetParamValue` / `SetParamValue`) clones with current values.
- **Transient DSP state** (oscillator phase, ADSR `Stage` / `Level`, SVF `z1` / `z2`, smoother current value, S&H `HeldValue`) **resets** when `Prepare(SampleRate)` is called by the snapshot's `Compile`. This is correct: every voice starts in a clean state.
- **Resource handles** (RtMidi device, file handles, OS-thread joiners) are **not cloneable**. Override `Clone()` to return `nullptr` on those nodes.

**Non-cloneable node list:**
- `FMidiInput` — owns an `RtMidiIn` instance and a callback thread. Cloning would attempt to open the same device twice. Returns `nullptr` from `Clone()`.
- `FVirtualKeyboard` — its UI state (held-note stack, mod-wheel slider) is per-instance. The user wouldn't expect each voice to have its own keyboard. Returns `nullptr`.
- `FOutput` — singleton already enforced. Returns `nullptr`.

`Compile()` rejects a `bPerVoice` flag on a non-cloneable node with a clear error.

### 1.4 Per-voice flag — model + UI

**Model:** `FNodeRecord` gains `bool bPerVoice = false`. Toggled via `FGraphModel::SetNodePerVoice(Id, bool)`. Setting it on a non-cloneable node is rejected with a return value of false; the call is silently ignored.

**UI:**
- Right-click on a node in the graph → context menu with "Per-voice" toggle (checkmark when set). Disabled / greyed out for non-cloneable types.
- Visual indication on the node header: a small per-voice badge (a "×N" overlay icon in the existing icon palette colour) drawn in `Editor.cpp`'s `BeginNode` block.
- Property panel shows the flag as a read-only checkbox under the type name (consistent place; user toggles via context menu, not the property panel — keeps the toggle near the node).

### 1.5 Polyphonic signals & edge validation

A signal is **polyphonic** if its source node is per-voice or `FVoiceAllocator`. Otherwise it's **monophonic** (global).

**Edge validation rules** (`AddLink` and `Compile` both enforce):

| From → To | Audio | Control |
|---|---|---|
| Mono → Mono | OK | OK |
| Mono → Per-voice | OK (broadcast: same buffer feeds all voice clones) | OK (broadcast) |
| Per-voice → Per-voice | OK (paired: voice-i's source feeds voice-i's destination) | OK (paired) |
| Per-voice → Mono | OK (sum across voices at the boundary) | **Rejected** (ambiguous: which voice's control value?) |

The "per-voice → mono Control" rejection is the only new validation rule. Audio summing at the boundary is the conventional "mix all voices into the master output" behaviour and needs no user intervention.

There is no "per-voice across voices" case in the user-edited graph — clones don't exist there. Cross-voice connections are an artefact of compilation, not a thing the user can draw.

### 1.6 `FVoiceAllocator` node

```
FVoiceAllocator
  inputs : —  (events arrive via the audio command queue; not graph edges)
  outputs: Gate, Frequency, Velocity, Note  (all Control, polyphonic)
  params : NumVoices (Choice: 1, 2, 4, 8 — default 8),
           Glide (ms, 0..2000, log) — for future portamento
```

Outputs read like normal Control ports in the graph editor (one pin per port). The compiler treats them as polyphonic.

Internally:
- Per-voice state struct: `{ Note, Velocity, bGate, AgeSamples, ReleaseStartedAtSample }`. Plain (non-atomic) members; touched only by the audio thread.
- Drains `NoteOn` / `NoteOff` commands from the queue at block start (extending `FAudioGraph::DrainCommands` — see §1.7).
- Voice-stealing logic per §1.8.
- Each block, writes per-voice Gate/Freq/Velocity/Note to its **N output buffers**. The compiler maps voice `i`'s downstream consumers to output channel `i`.

### 1.7 `NoteOn` / `NoteOff` commands

Extend `EAudioCommand`:

```cpp
enum class EAudioCommand : uint8_t
{
    SetParam,
    NoteOn,    // Data1 = MIDI note 0..127, Data2 = velocity 0..127
    NoteOff,   // Data1 = MIDI note
};
```

Reuse the existing `FAudioCommand` shape — `NodeId` ignored for NoteOn/Off (broadcast to all voice allocators in the snapshot), `ParamIndex` repurposed as the note number, `Value` as velocity (cast from 0..127).

Or cleaner: extend `FAudioCommand` with a small `union` / dedicated fields. Pragmatic choice for v1: reuse existing fields with documented repurposing — keeps the ring layout unchanged.

**Producers:** `FMidiInput` (already drains real MIDI; just adds a queue push alongside its existing Control output writes), `FVirtualKeyboard` (its `PressNote` / `ReleaseNote` push commands).

**Consumer:** `FAudioGraph::DrainCommands` dispatches `SetParam` to the addressed node (existing behaviour) and broadcasts `NoteOn` / `NoteOff` to every `FVoiceAllocator*` it can find (cached as a `std::vector<FVoiceAllocator*>` in the compiled snapshot, populated alongside `NodeById`).

For v1 with one allocator per graph, broadcast is the right semantics. Multi-allocator graphs are out of scope; broadcast still does something sensible (every allocator hears every event).

### 1.8 Voice stealing policy

Per-voice state tracks `bGate` (currently held) and `AgeSamples` (samples since note-on).

On `NoteOn(N, V)`:
1. **Same-note re-trigger first.** If any voice is currently holding note N, retrigger it (`bGate = true`, reset `AgeSamples`, update Velocity). No allocation.
2. **Free voice next.** If any voice has `bGate == false` AND its envelope has fully released (proxy: `AgeSamples` since `bGate` went false exceeds `ReleaseThresholdSamples`, default ~100 ms), allocate the *oldest* such voice.
3. **Released-but-still-tailing voice.** Otherwise, allocate the oldest voice with `bGate == false` (cuts off the release tail; ADSR's existing retrigger-from-current-level keeps the click out).
4. **Steal a held voice.** Otherwise, steal the *oldest* held voice (the one with the largest `AgeSamples`).

On `NoteOff(N)`:
- Find the voice holding note N. Set `bGate = false`. The ADSR downstream sees the gate drop and starts releasing; the voice stays "occupied" for stealing-priority purposes until either time passes or it gets stolen.

**No same-note duplicate voices.** Pressing C4 while C4 is already held re-triggers, doesn't allocate a second voice. Avoids stack-up on stuck-key situations.

**No held-note resurrection on voice steal.** If the user is holding 9 notes and releases one — we don't re-allocate the stolen note. Stolen is gone.

### 1.9 Patch save format

`FNodeRegistration` JSON gains one optional field per node:

```json
{ "id": 7, "type": "Oscillator", "x": ..., "y": ..., "params": {...}, "per_voice": true }
```

Loader reads with default `false` if missing — back-compat with v1 patch files. Schema version stays at 1 since this is purely additive. `FAudioCommand`'s `NoteOn` / `NoteOff` are not in the save format (events, not state).

### 1.10 Compiled snapshot lifetime

Cloned nodes are owned by the snapshot only. Specifically:

- `FAudioGraph::OrderedNodes` holds `shared_ptr<INode>` to all nodes used in the audio path — original nodes for global, fresh clones for per-voice.
- The original per-voice node (the one in `FGraphModel::Nodes`) does **not** appear in `OrderedNodes` after a polyphonic compile. It's a template only.
- `NodeById` only maps original IDs from `FGraphModel`; clones don't get IDs and aren't queue-addressable. SetParam routes to the original; `Compile` propagates the value to all clones (via the same param-by-name copy in `Clone()`'s implementation).

Implication: dragging a slider on a per-voice oscillator pushes one `SetParam` (to the original). The original's `SetParamValue` fires, but the original isn't in the audio path. We need to fan it out.

**Fix:** the audio thread's `DrainCommands`, when handed a `SetParam` for a per-voice original, also writes the param to every clone. Cheap (small N) and keeps the queue API uniform. Implementation: `NodeById` value type changes from `INode*` to a small struct `{ INode* Primary; std::vector<INode*> Voices; }`. For non-per-voice nodes, `Voices` is empty.

---

## 2. Sub-phases for 3E implementation

```
3E-1 (Clone foundation) ──► 3E-2 (Per-voice flag) ──► 3E-3 (FVoiceAllocator) ──► 3E-4 (Compile partition) ──► 3E-5 (Tests + integration)
```

### Phase 3E-1 — `Clone()` foundation (~half day)

- Add `INode::Clone()` virtual with the default implementation from §1.3 (in `Node.h`, calls `MakeNodeByTypeName`).
- Override `Clone()` to return `nullptr` on `FMidiInput`, `FVirtualKeyboard`, `FOutput`.
- Catch2 tests: cloning every cloneable node preserves param values; cloning a non-cloneable returns `nullptr`; cloned node's transient state resets on `Prepare`.

### Phase 3E-2 — Per-voice flag (~1 day)

- `FNodeRecord::bPerVoice` field, default false. `FGraphModel::SetNodePerVoice(Id, bool)` method that rejects non-cloneable nodes (uses `Clone()` returning non-null as the cloneability test).
- Right-click context menu in `Editor.cpp` with the toggle. Greyed for non-cloneable types.
- Visual badge in the node header (small "×N" overlay where N is current `NumVoices`, or just a per-voice icon).
- Patch serialiser: write `per_voice: true` when set; loader reads with default false.
- Tests: model-level toggle, save/load roundtrip preserves the flag.

### Phase 3E-3 — `FVoiceAllocator` + NoteOn/NoteOff commands (~1.5 days)

- `EAudioCommand::NoteOn` / `NoteOff` variants. Document the field repurposing.
- `FVoiceAllocator` class (`TNodeBase<0, 4>` × NumVoices on outputs — see §1.6). Stub voice-allocator logic: drain note events, write per-voice gate/freq/velocity buffers, no stealing yet (just first-free-voice allocation).
- `FAudioGraph` gains `std::vector<FVoiceAllocator*> Allocators` cached at compile time. `DrainCommands` dispatches NoteOn/NoteOff to all of them.
- `FMidiInput` and `FVirtualKeyboard` push NoteOn/NoteOff alongside their existing Control output behaviour.
- Register `FVoiceAllocator` in `NodeRegistry.cpp` with icon + tooltip.
- Tests: allocator's first 8 simultaneous notes go to 8 voices; `NoteOff` drops the right voice's gate; broadcast reaches multiple allocators.

### Phase 3E-4 — Compile partition + cloning (~2 days)

The hard one.

`FGraphModel::Compile` changes:

1. **Reachability** as today (reverse DFS from `FOutput`).
2. **Polyphony classification:** mark each reachable node as Mono or PerVoice. Per-voice = `bPerVoice == true`. Initially `FVoiceAllocator` itself is a PerVoice **source** (its outputs are polyphonic) but the node itself isn't cloned.
3. **Edge validation** (§1.5): walk all reachable links, check the table; reject the compile if any per-voice → mono Control link exists. Return an empty `FAudioGraph` and surface the error to the UI (status bar message).
4. **Build voice clones:** for each per-voice node, instantiate `NumVoices` clones via `Clone()`. Store as a `std::vector<std::shared_ptr<INode>>` keyed by original NodeId.
5. **Re-plumb buffers:** for each link `From → To`,
   - If both Mono: as today.
   - If From PerVoice (or `FVoiceAllocator`) and To PerVoice: voice-i's `From` clone → voice-i's `To` clone.
   - If From Mono and To PerVoice: same `From` buffer → all voice clones of `To`.
   - If From PerVoice and To Mono Audio: synthesize an `FVoiceMixer` helper node (internal, not user-visible) that sums N inputs into one output. Wire all voice clones' outputs into the mixer; mixer's output → `To`.
6. **OrderedNodes:** topologically sort the now-larger node set. Voice clones come before global summation nodes.

`FVoiceMixer` is an internal helper not exposed in the registry; it's created only by the compiler. Lives in `src/dsp/internal/VoiceMixer.h`.

Tests: compile a polyphonic graph; verify `OrderedNodes` includes 8 oscillator clones and a synthesised mixer; verify a per-voice → mono Control link is rejected.

### Phase 3E-5 — Voice stealing + tests + integration (~1.5 days)

- Implement the policy from §1.8 in `FVoiceAllocator`.
- Update the seeded default patch to demonstrate polyphony: `VirtualKbd → FVoiceAllocator → Osc (per-voice) + ADSR (per-voice) → Out`.
- Catch2 tests:
  - 8 simultaneous notes hit 8 distinct voices.
  - 9th note steals oldest released first, then oldest held.
  - Same-note re-trigger uses existing voice (no double-allocation).
  - ADSR releases independently per voice.
  - Glide param wiring placeholder (no portamento yet — Phase 4).

---

## 3. Sequencing & budget

```
Day 1:    3E-1 + start 3E-2
Day 2-3:  3E-2 + 3E-3 (start)
Day 4-5:  3E-3 finish + 3E-4 start
Day 6-7:  3E-4 finish
Day 8-9:  3E-5
```

**Rough total: 8–10 days** (the original plan's 5–7 was optimistic). The compile partition step is the schedule risk — see Risks below.

If the partition algorithm proves nasty, the fallback is a stripped 3E that requires the *entire* downstream graph from `FVoiceAllocator` to `FOutput` to be per-voice (no mixing global modulation in). That removes the validation/mixer machinery and ships a working 8-voice synth without flexible mod routing. Park advanced routing for Phase 4.

---

## 4. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Partition algorithm in 3E-4 has corner cases I haven't thought through (e.g. branches reconverging, diamonds with mixed polyphony) | Stripped fallback: enforce "everything from VoiceAllocator to Output is per-voice" via validation; defer mixing until 3F. Keeps 3E shippable. |
| Voice clones leak (e.g. if recompile creates clones but the old snapshot's clones don't get released cleanly) | Clones live in `OrderedNodes`'s `shared_ptr`s; old snapshot's `shared_ptr<FAudioGraph>` is dropped on swap; ref count goes to 0; nodes destruct. Same lifecycle as today's nodes. Confirm with a test: hold a `weak_ptr` to a clone, recompile, verify it expires. |
| `NoteOn` / `NoteOff` field repurposing in `FAudioCommand` is confusing | Document carefully in `AudioCommand.h` and add named accessor methods (`MakeNoteOn(Note, Vel)`, `MakeNoteOff(Note)`, `GetNoteOnNote()`). |
| 8 voices × 2 oscillators × ADSR + filter eats too much CPU on weaker machines | Profile after 3E-5. Drop to 4 voices if needed. Consider SIMD-ing the oscillator inner loop in Phase 4. |
| Cloning `FAdsr` mid-note creates audible glitch (cloned voice has reset state but original was mid-attack) | Recompile is a deliberate user action (graph edit, NumVoices change). Audible glitch on graph edit is acceptable v1 behaviour — same as today. |
| User toggles per-voice flag on a node mid-play | Triggers a recompile. May glitch. Acceptable. Document. |
| Missing per-voice flag UI affordance — user can't tell what's per-voice | The header badge (§1.4) is mandatory; without it the feature is invisible. |
| Voice stealing clicks because release tail abruptly cut | ADSR retrigger-from-current-level is already in (Phase 2). New attack starts from old level → no discontinuity. Test with a long-release patch. |

---

## 5. What stays deferred

- **Polyphonic cables** — approach (a) from `PLAN-PHASE-3.md` §1.5. If approach (b) shows fundamental limitations during 3E (e.g. users want different routing per voice), revisit then.
- **MPE** (per-note pitch-bend, per-note pressure). Requires extending NoteOn/Off with a NoteId. Out of scope.
- **Portamento / glide.** Param shape exists on `FVoiceAllocator`; implementation is Phase 4.
- **Live re-voicing** without recompile. Phase 4+.
- **Per-voice modulation routing UI.** Per-voice is a node-level flag, not a connection-level one. If users want voice 1's filter to cutoff differently from voice 2's, they get a Phase 4 redesign.
- **More than one `FVoiceAllocator` in a graph.** Broadcast handles it sensibly but no UI affordance to steer events. Phase 4.
- **Sample-accurate NoteOn / NoteOff.** Events still drain at block start; quantisation to sample 0 of the block is fine for human play. Sample-accurate MIDI was already deferred.
