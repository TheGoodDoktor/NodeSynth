# NodeSynth — Backlog

Plans that have been written and reviewed but not yet picked up. Each entry points at its plan doc; the doc holds the full design, sub-phases, and risks.

When work starts on an item, leave the entry here until it ships, then move it to the "Shipped" section at the bottom (or remove it if the plan doc itself records the outcome).

---

## Open

### Modulation Matrix
- **Plan:** [`PLAN-MOD-MATRIX.md`](PLAN-MOD-MATRIX.md)
- **Summary:** New `FModulationMatrix` node — 8 control sources × 8 control destinations with per-cell depth + per-output offset. Replaces the `LFO → Scale → Add → ...` chains that pile up on heavily-routed patches. Custom 8×8 grid UI in the property panel.
- **Estimate:** ~2–3 days, single PR.
- **Status:** Plan complete. Not yet started.

### Subgraphs
- **Plan:** [`PLAN-SUBGRAPHS.md`](PLAN-SUBGRAPHS.md)
- **Summary:** New `FSubgraph` node that wraps an internal graph and exposes user-declared input/output pins. Edited via dive-into-with-breadcrumb navigation; saved as standalone `.nspg` assets and embedded in patches. Compiles via inline macro-expansion so the audio path stays as cheap as a hand-wired equivalent. Mental model: Unreal Blueprint Functions.
- **Estimate:** ~2–3 weeks, six sub-phases (SG.1–SG.6).
- **Status:** Plan complete. Not yet started.

### MIDI CC Source Node
- **Plan:** [`PLAN-MIDI-CC.md`](PLAN-MIDI-CC.md)
- **Summary:** New `FMidiCC` node — emits a smoothed Control output `[Min, Max]` reading from an assigned MIDI CC. Distinct from per-param MIDI Learn (which binds CC → one specific param); this node makes the CC a graph-routable Control source so it can be remapped, summed, fed to a mod matrix, etc. Requires fanning the `FMidiDeviceManager` callback's CC stream into a second SPSC ring drained on the audio thread.
- **Estimate:** ~half a day, four sub-phases (CC.1–CC.4) in one PR.
- **Status:** Plan complete. Not yet started.

---

## Shipped

(empty — populate as items land)
