# NodeSynth — MIDI CC Node Plan

A new `FMidiCC` node that emits a smoothed Control output reading from an assigned MIDI CC. Sibling to `FLfo` / `FSampleHold` under the **Modulation** category. Distinct from the existing per-param **MIDI Learn** mapping: that binds a CC to one specific param via the audio-command queue; this exposes the CC as a graph-routable Control source so it can be remapped, summed, fed into a Sample-and-Hold, used as a mod matrix source, and so on.

**Entry state:** `FMidiDeviceManager` is the project-level MIDI subsystem. It owns the `RtMidiIn`, splits the callback stream by status nibble: notes go to a SPSC ring drained on the audio thread (delivered to all `FVoiceAllocator`s), CCs go to a separate SPSC ring drained on the UI thread (consumed by MIDI Learn). The audio thread currently has no path to CC values.

**Goal:** drop a `FMidiCC` node into the graph, assign a CC# (manually or via Learn), wire its output into any Control input, and watch a hardware controller sweep the destination in real time. Output is in `[Min, Max]` with one-pole smoothing.

**Final deliverable that defines "done":**
- `FMidiCC` registered in the palette under **Modulation** with a custom icon.
- `FMidiDeviceManager` fans CC events to both the UI ring (existing) and a new audio ring (new), so multiple `FMidiCC` nodes can read CCs on the audio thread.
- Property panel: CC# slider, Channel choice (Omni / 1..16), Min / Max sliders, Learn button, "last value" indicator.
- Smoothing time constant ~5 ms (faster than the standard 30 ms — CC steps are coarse and the user expects responsive feedback).
- One showcase preset using a CC node to drive filter cutoff.

---

## 1. Decisions to lock first

### 1.1 Where the CC events come from

**Decision: the callback fans CC events into two SPSC rings — the existing UI-thread ring and a new audio-thread ring.**

`FMidiDeviceManager`'s callback already splits notes vs CCs. Today CCs go to the UI ring. We add a second ring (`AudioCcRing`, capacity ~256) and the callback pushes every CC into both. UI drains its copy for MIDI Learn (unchanged); the audio thread drains the new one at the start of each `Process` and dispatches to every `FMidiCC` node in the snapshot.

**Why not share one ring with two consumers?** SPSC is single-consumer by definition. MPMC would work but needs a real lock-free queue and changes the contention story; a duplicate SPSC is simpler, and CC traffic is light (typically < 1 kHz total).

**Why not bridge UI→audio?** Adds frame-rate latency (~16 ms at 60 fps) and a synchronisation hop. The whole point of an audio-rate CC source is responsive playback.

### 1.2 Dispatch to nodes

**Decision: a flat list of `FMidiCC*` pointers on `FAudioGraph`, populated during Compile.**

Mirrors how `FAudioGraph` already keeps a `CcMappings` table for per-param MIDI Learn and a `VoiceAllocators` list. After draining the audio CC ring once per block, the manager visits every `FMidiCC*` and forwards events that match the node's `(CC#, Channel)` filter.

Per-voice cost: zero. Even with a polyphonic patch, there's only one logical CC source per node — clones share the dispatched value.

### 1.3 Param surface

```
CC        Float  0..127      default 1     (clamped to int)
Channel   Choice Omni/1..16  default Omni
Min       Float  -1..+1      default 0     (no clamping — can exceed; user knows what they want)
Max       Float  -1..+1      default 1
Smooth    Float  0.5..200 ms log  default 5
```

`CC` could be `Int` kind, but NodeSynth doesn't have one — Float clamped + cast on the audio side is the existing convention (see `FOscillator::Param_Shape`). `Channel` is a 17-entry Choice (Omni first) for compactness.

`Min` / `Max` are intentionally not clamped to a fixed range — the user might want a CC that drives filter cutoff `200..8000 Hz` directly, without a downstream `Scale`. Per-node combine of CC + remap is the explicit ask from the user; honour it.

`Smooth` exposed as a slider rather than a fixed constant because (a) CC step quantisation makes the right value patch-dependent and (b) some destinations (filter cutoff) want fast response, others (oscillator pitch) want longer to mask zipper.

### 1.4 Learn

**Decision: a "Learn" button in the property panel; UI thread captures the next CC event for this node.**

`FGraphEditorPanel` already drains the UI CC ring once per frame for the existing per-param MIDI Learn UX. Extending it: when an `FMidiCC` node has Learn active, the same drain assigns the next received CC# / Channel to that node's params (via the existing dual-write — direct `SetParamValue` plus queued `SetParam` command).

Only one node can be in Learn mode at a time — clicking Learn on a second node cancels the first's. Same convention as the existing Learn flow.

### 1.5 Initial output value

**Decision: output equals `Min` (treating "no CC received yet" as raw value 0).**

Alternative (output midpoint of `Min`/`Max`) would mean every patch loads with a partial-state filter cutoff or whatever — surprising. Min mirrors the standard "zero-init" convention.

The smoother starts at `Min` too, so the first CC received ramps cleanly from `Min` to its target rather than starting from zero in absolute terms.

### 1.6 Per-voice

**Decision: cloneable via the default `Clone()`, but per-voice flag is a no-op for this node — every clone reads the same CC value.**

We don't reject the per-voice flag (avoids special-casing in `SetNodePerVoice`), we just don't gain anything by setting it. The per-voice clones each receive the same dispatched CC stream. Document in the registry tooltip.

### 1.7 Palette + icon

**Category:** Modulation. **Icon:** small "CC" text or a horizontal slider stylisation. The `MIDI Input` node already exists with a 5-pin DIN icon — we want something visually distinct but in the same colour family.

**Decision:** small horizontal slider track with a knob marker at ~1/3 position, in `ColInput` purple (matches MIDI Input + Voice Allocator).

---

## 2. Sub-phases

```
CC.1 (FMidiDeviceManager audio CC ring + dispatch)   ──┐
CC.2 (FMidiCC node + smoother)                       ──┼── ship
CC.3 (Property panel UI + Learn button)              ──┤
CC.4 (Tests + registry + showcase preset)            ──┘
```

Four small phases, ~half-day total. Single PR.

### CC.1 — Audio CC ring + dispatch (~1 hour)

`FMidiDeviceManager`:
- Add `FCcRing AudioCcRing` (capacity 256) alongside the existing UI `CcRing`.
- In the RtMidi callback's CC branch, push to *both* rings.
- Add `DrainAudioCcEvents(Visitor)` for the audio thread, mirroring the existing UI-side `DrainCcEvents`.
- Add `void SetMidiCcNodes(std::vector<FMidiCC*>)` so the audio-thread `Process` can broadcast the drain to every `FMidiCC` in the current snapshot. Populated during `Compile` the same way `SetVoiceAllocators` is.

**Done when:** an existing test injecting a synthetic CC into the manager observes both rings receiving the event.

### CC.2 — `FMidiCC` node (~2 hours)

`src/dsp/MidiCC.h` — header-only node, `TNodeBase<0, 1>` (no inputs, one Control output).

State:
- `std::atomic<uint8_t> CcNumber`, `std::atomic<int8_t> ChannelFilter` (-1 = omni, 0..15 = channel index 0-based internally)
- `std::atomic<float> Min`, `Max`, `SmoothMs`
- Audio-thread-only: latched raw value (`uint8_t LastRaw`, default 0) and a `FOnePoleSmoother`.

Process:
1. Snapshot params at block start.
2. Manager has already drained the CC ring and called `OnCcEvent(channel, cc, value)` on this node for matching events; `LastRaw` is up to date.
3. For each sample: smoother target = `Min + (LastRaw / 127.0) × (Max - Min)`, output = `Smoother.Tick()`.

`OnCcEvent(uint8_t Channel, uint8_t Cc, uint8_t Value)`: called once per drained event from the audio thread. Filter by CC# and Channel; on match, update `LastRaw`.

**Done when:** a unit test pushes a CC into the manager → drain → asserts the node's output ramp settles to the expected value.

### CC.3 — Property panel UI + Learn (~1 hour)

Standard Float / Choice widgets handle CC / Channel / Min / Max / Smooth. The custom UI overlay adds:
- A "Learn" button next to the CC# slider. Active state: the button glows, status text says "Move a controller…"
- A small text label "Last: 67" showing the last received raw value (atomic read).

`FGraphEditorPanel`'s existing CC drain (already wired up for per-param Learn) gains a branch: if the active learn target is an `FMidiCC` node, set `Param_CC` and `Param_Channel` on the node directly + push through the audio queue.

**Done when:** clicking Learn → moving a hardware knob → CC# auto-fills and the output starts tracking.

### CC.4 — Tests + registry + showcase (~1 hour)

`tests/MidiCC.test.cpp`:
1. Default state: output = `Min` after smoothing settles.
2. Direct injection: simulate `OnCcEvent` with value 0 → output = `Min`; value 127 → output = `Max`; value 64 → output ≈ midpoint.
3. Channel filter: events on a different channel are ignored.
4. CC# filter: events for a different CC# are ignored.
5. Min/Max range: with `Min=200, Max=8000`, raw 64 → output ≈ 4173.
6. Smoothing: a step from Min to Max produces a ramp, not an instant transition.
7. Per-voice clone: `Clone()` returns a non-null instance with identical params.

Register in `NodeRegistry.cpp` under category **Modulation**:

```cpp
{ "MidiCC", "MIDI CC",
  "Reads a MIDI CC from the project-level MIDI device and outputs a\n"
  "smoothed Control value in [Min, Max]. Click Learn to assign a CC by\n"
  "moving a hardware controller. Per-voice flag is a no-op — every\n"
  "voice reads the same CC.",
  "Modulation",
  []() -> std::shared_ptr<INode> { return std::make_shared<FMidiCC>(); },
},
```

Add a slider-with-knob icon dispatch in `NodeIcons.cpp`, ~10 lines, `ColInput` accent.

Showcase preset `Lead/CC Filter Lead`: polyphonic saw lead, an `FMidiCC` node assigned to CC#74 (most synths' "filter cutoff" by convention) with Min=200, Max=6000, drives an `FSvf` Cutoff input. Drag a knob on a controller, hear the filter sweep.

**Done when:** the showcase loads, hardware knob movement audibly sweeps the filter, the test suite stays green.

---

## 3. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Two CC rings means the callback CPU ~doubles for CC traffic | CC traffic is bounded at ~1 kHz on a busy controller; doubling that is still negligible. The callback runs on RtMidi's thread, not audio, so even if it spiked we'd be fine. |
| Audio CC ring fills under sustained controller wiggling and drops events | Capacity 256 events, drained every block (64 samples ≈ 1.3 ms at 48 kHz). Sustained throughput would have to exceed ~190k CCs/sec to overflow. Practical max from a hardware controller is ~1k/sec. Headroom is enormous. |
| Two `FMidiCC` nodes assigned to the same CC#: which wins? | Both update independently. The dispatch loop visits every node, each filters by its own CC#/Channel. Two nodes on the same CC produce identical output streams — by design. |
| Patch loaded with a Learn-active state | Learn is UI-thread-only state; never persisted. New patches load with Learn off. |
| User assigns CC# 0 (Bank Select MSB) — usually you don't want a synth control on it | Allow it. Bank Select isn't a sacred reserve in NodeSynth's context. Document if users hit it as a foot-gun. |
| Disconnect of MIDI device mid-patch | Existing FMidiDeviceManager handles this gracefully — when the device closes, ring stops receiving events; nodes hold their last received value. |

---

## 4. What stays deferred

- **NRPN / 14-bit CCs.** Standard CCs are 7-bit. 14-bit precision (CC + LSB pair) avoids stair-stepping on filter cutoffs. v2 — adds a "14-bit pair" choice.
- **Bipolar reception** (CC 64 = centre, 0/127 = extremes). Easy to fake with `Min = -1, Max = +1` for a centre-zero CC; native unipolar is the spec.
- **Curve / shaping** (linear vs exp vs logarithmic mapping inside Min..Max). Currently linear. Downstream `Scale` + `Multiply` chain covers it.
- **Per-CC-source learn-from-MIDI-file** (record automation, replay). Big feature; separate plan.
- **Pitch bend / aftertouch / channel pressure as additional source nodes.** Same architectural pattern; add as `FMidiPitchBend` / `FMidiAftertouch` once the CC node validates the approach.
- **Multiple CC inputs on one node** (combine CC#1 + CC#7 into one output). Composable downstream with `Add` / `Scale`; not foundational.

---

## 5. Why this lands fast

- The MIDI infrastructure exists. `FMidiDeviceManager` already parses messages, classifies notes vs CCs, and dispatches. We add one ring and one drain hook.
- The smoother / param-snapshot pattern is identical to every other node.
- The Learn UX has an existing template (per-param MIDI Learn already binds via UI-thread CC drain).
- One test file, one new node, one new ring.

Estimate: **~half a day end-to-end**, including tests, registry, icon, and the showcase preset. Same shape and size as `FConstant` / `FGateButton` — small, focused, leverages existing plumbing.
