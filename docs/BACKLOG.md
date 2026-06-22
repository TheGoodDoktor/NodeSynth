# NodeSynth — Backlog

Plans that have been written and reviewed but not yet picked up. Each entry points at its plan doc; the doc holds the full design, sub-phases, and risks.

When work starts on an item, leave the entry here until it ships, then move it to the "Shipped" section at the bottom (or remove it if the plan doc itself records the outcome).

---

## Open

_(none)_

---

## Shipped

### Subgraphs
- **Plan:** [`PLAN-SUBGRAPHS.md`](PLAN-SUBGRAPHS.md)
- **Summary:** `FSubgraph` node wrapping an internal graph with user-declared input/output pins. Edited via dive-into-with-breadcrumb navigation; saved as standalone `.nspg` assets and embedded in patches; compiled by inline macro-expansion (zero runtime overhead). All six sub-phases shipped (SG.1 definition + serialization, SG.2 boundary nodes + compile expansion, SG.3 dive-in editor + breadcrumb, SG.4 pin management UI, SG.5 patch embedding + `.nspg` asset library + drag-drop, SG.6 bundled `StereoFilter` + `Lead/Subgraph Demo` preset + Ctrl+G "Make Subgraph from Selection" + Esc-to-pop). Built on `feature/subgraphs`. Deferred to a v2: per-level undo/redo inside subgraphs (§1.12), full round-trip of *nested* subgraph instances' internal links, "Reload from Asset".

### MIDI CC Source Node
- **Plan:** [`PLAN-MIDI-CC.md`](PLAN-MIDI-CC.md)
- **Summary:** `FMidiCC` node emitting a smoothed Control output `[Min, Max]` driven by an assigned MIDI CC. Audio-side CC dispatch via a parallel SPSC ring in `FMidiDeviceManager`. Property panel UI with Learn button + live raw-value indicator. Showcase preset `Lead/CC Filter Lead`.

### Modulation Matrix
- **Plan:** [`PLAN-MOD-MATRIX.md`](PLAN-MOD-MATRIX.md)
- **Summary:** `FModulationMatrix` — 8×8 routing node, 64 depth knobs + 8 per-output offsets, per-output 30 ms one-pole smoothers, 72 hidden params surfaced via custom UI grid (right-click cells for Zero / Invert; columns dim when source is unwired). Showcase preset `Pad/Matrix Routing Pad` combining two LFOs into filter cutoff + resonance.
