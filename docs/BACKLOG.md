# NodeSynth — Backlog

Plans that have been written and reviewed but not yet picked up. Each entry points at its plan doc; the doc holds the full design, sub-phases, and risks.

When work starts on an item, leave the entry here until it ships, then move it to the "Shipped" section at the bottom (or remove it if the plan doc itself records the outcome).

---

## Open

### Subgraphs
- **Plan:** [`PLAN-SUBGRAPHS.md`](PLAN-SUBGRAPHS.md)
- **Summary:** New `FSubgraph` node that wraps an internal graph and exposes user-declared input/output pins. Edited via dive-into-with-breadcrumb navigation; saved as standalone `.nspg` assets and embedded in patches. Compiles via inline macro-expansion so the audio path stays as cheap as a hand-wired equivalent. Mental model: Unreal Blueprint Functions.
- **Estimate:** ~2–3 weeks, six sub-phases (SG.1–SG.6).
- **Status:** Plan complete. Not yet started.

---

## Shipped

### MIDI CC Source Node
- **Plan:** [`PLAN-MIDI-CC.md`](PLAN-MIDI-CC.md)
- **Summary:** `FMidiCC` node emitting a smoothed Control output `[Min, Max]` driven by an assigned MIDI CC. Audio-side CC dispatch via a parallel SPSC ring in `FMidiDeviceManager`. Property panel UI with Learn button + live raw-value indicator. Showcase preset `Lead/CC Filter Lead`.

### Modulation Matrix
- **Plan:** [`PLAN-MOD-MATRIX.md`](PLAN-MOD-MATRIX.md)
- **Summary:** `FModulationMatrix` — 8×8 routing node, 64 depth knobs + 8 per-output offsets, per-output 30 ms one-pole smoothers, 72 hidden params surfaced via custom UI grid (right-click cells for Zero / Invert; columns dim when source is unwired). Showcase preset `Pad/Matrix Routing Pad` combining two LFOs into filter cutoff + resonance.
