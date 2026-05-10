# NodeSynth — Modulation Matrix Plan

A new `FModulationMatrix` node, sibling to `FLfo` / `FSampleHold` / `FSequencer` under the **Modulation** category. One node with 8 control inputs and 8 control outputs; each output is a depth-weighted sum of the inputs plus a per-output offset. Replaces the `LFO → Scale → Add → ...` chains that pile up on patches with serious routing.

**Entry state:** Wavetables done (WT.1-7). Palette grouped by category, 30 bundled presets. Existing modulation nodes are `FLfo`, `FSampleHold`, `FClock`, `FSequencer`. Routing one source to multiple destinations currently means manual fan-out + per-destination `Scale` + `Add`. Two LFOs into three destinations with mix levels is already 6+ ancillary nodes.

**Goal:** drop a `Modulation Matrix` into the graph, wire several CV sources in, wire several destinations out, and adjust an 8×8 grid of depth knobs to mix the routing. Same UX shape as a vintage hardware modular's patchbay.

**Final deliverable that defines "done":**
- `FModulationMatrix` registered in the palette under **Modulation** with a custom icon.
- 8 Control inputs (`Src1`..`Src8`) and 8 Control outputs (`Dst1`..`Dst8`).
- Custom property-panel UI with an 8×8 grid of depth sliders + an 8-wide row of per-output offsets.
- Per-voice flag works (matrix is cloneable; routing is paired by voice when fed from per-voice sources).
- Showcase preset: a polyphonic patch where the matrix routes `LFO1` to filter cutoff and pan AND routes the `ADSR` to a second filter cutoff, all from one node.

---

## 1. Decisions to lock first

### 1.1 Size: 8 × 8

**Decision: 8 sources × 8 destinations.**

Synth-history reference points:
- Korg MS-20 patch panel: ~10 routing points
- Serum mod matrix: 4 grouped slots, but realistically dozens are addressable
- Vintage modular hardware: usually 8–16 entries in a "mod busbar"

8×8 is the sweet spot for a typical NodeSynth patch — most polyphonic patches use 1–3 modulation sources (LFO, ADSR, key-tracking) and 2–4 modulation destinations (filter cutoff, amplitude trim, pan, position, pitch). 8×8 leaves room for elaborate patches without the UI density of 16×16 (256 cells is unmanageable).

The matrix is sized at compile time — `static constexpr uint32_t NumSources = 8` and `NumDestinations = 8`. A larger matrix would be a separate node type, not a parameterised resize (compile-time sizing keeps `Process` allocation-free and the param layout fixed).

### 1.2 Routing math

**Decision: per-output linear combination plus offset.**

```
Dst_i[s] = Offset_i + Σ_j (Src_j[s] × Depth_ij)
```

- `Src_j[s]` is the input buffer's sample s. Disconnected inputs read as 0.
- `Depth_ij` is the depth knob at row i, column j, range `[-1, +1]`.
- `Offset_i` is the per-destination offset, range `[-1, +1]`. Lets a destination produce a non-zero default when all inputs are silent (e.g. "filter cutoff at 0.5 with LFO modulation around it").

CPU: `8 × (8 mul + 8 add + 1 add) ≈ 144` flops per sample, or ~7 M flops/sec at 48 kHz. Negligible.

### 1.3 Smoothing strategy

**Decision: one `FOnePoleSmoother` per output (8 total), applied to the post-sum value.**

Per-cell smoothers would be more "correct" but cost 64 smoothers and don't audibly differ from per-output smoothing — the user only ever moves one cell at a time and the per-output sum captures that change. 8 smoothers is the right cost/quality knee.

Time constant: `30 ms`, matching the convention in `FGain` / `FOscillator::AmpSmoother`.

### 1.4 Param layout

```
Depth_<i>_<j>      Float  -1..+1   default 0.0   for i in 0..7, j in 0..7   (64 params)
Offset_<i>         Float  -1..+1   default 0.0   for i in 0..7              (8 params)
```

Total 72 params. All flagged `bHidden = true` so the standard widget loop skips them — the custom UI renders the grid.

This mirrors `FSequencer`'s hidden-param + custom-UI pattern (it has 64 hidden per-step params). Save/load round-trips them via `PatchSerializer` automatically.

Param indexing: `Param_DepthBase + i*NumSources + j` and `Param_OffsetBase + i`. Constants in the node's enum so test code doesn't index by raw integer.

### 1.5 Per-voice behaviour

**Decision: cloneable via the default `INode::Clone()`.**

The matrix has only atomic-float state — no buffers, no FFT-built data. The default Clone (fresh instance + param-copy by name) does the right thing.

Per-voice flagging works as expected:
- **Mono matrix**: one shared routing. Mono CV sources only. (Per-voice → mono Control rejected by Compile.)
- **Per-voice matrix**: per-voice clones. Each voice runs its own copy; per-voice sources (e.g. ADSR) feed the corresponding voice's matrix.

Mixing source types: a mono LFO can feed a per-voice matrix's `Src1` (broadcast), while a per-voice ADSR feeds `Src2` paired by voice. That's the standard mono→per-voice / per-voice→per-voice graph behaviour — nothing matrix-specific.

### 1.6 UI: 8×8 grid

**Decision: ImGui `SliderFloat` cells in a fixed-width grid; per-output offset row beneath.**

Layout (~400 px wide × ~280 px tall):

```
                Dst0   Dst1   Dst2  ...  Dst7
       Src0   [ slider ] [ ... ] ...
       Src1   [ ... ]
       ...
       Src7   [ ... ]
       Off    [ ... offsets ... ]
```

Each cell is a vertical bipolar slider, ~36 px wide × ~16 px tall, range `-1..+1`. Right-click context menu: **Zero**, **Invert**. Color tint reflects sign (positive = teal, negative = red) and magnitude (saturation). Disabled-cell rendering for slots whose corresponding input port has nothing wired — visual cue that the cell will read zero regardless of depth.

Source/destination labels are static `Src1..8` / `Dst1..8` for v1. v2 enhancement: optional per-row/column string label that the user types (mirrors how a hardware modular patch panel gets hand-labelled with masking tape).

Slider drag coalescing already works for hidden params via the existing `IsItemActivated` / `IsItemDeactivatedAfterEdit` pattern in `Editor.cpp`. One undo entry per drag.

### 1.7 Where it lives in the palette

**Decision: category `Modulation`.**

Companions: `LFO`, `Sample & Hold`, `Clock`, `Sequencer`. The matrix is the routing layer those sources feed into.

### 1.8 Icon

**Decision: a 3×3 dot grid in `ColMath` accent.**

Visually distinct from the other Modulation icons (LFO is a sine, Clock is a clock face, Sequencer is bar-step heights, S&H is stairs). A grid of dots reads as "matrix" without ambiguity. Same colour family as the math nodes since the matrix is fundamentally a linear transformation of CV inputs.

### 1.9 What about audio-rate sources?

**Decision: ports are typed `EPortType::Control`, not `Audio`.**

Routing audio-rate signals through a depth-weighted matrix would let you build interesting cross-modulation effects, but it cuts both ways: (a) the existing graph type-check rejects Audio→Control links, so we'd need to widen ports or split into AudioMatrix/ControlMatrix variants, and (b) audio-rate FM is rare in this synth's design space. Defer.

The matrix outputs are Control too — they feed parameter modulation inputs (e.g. `SVF.Cutoff`, `WavetableOscillator.Position`, etc.).

---

## 2. Sub-phases

```
MM.1 (FModulationMatrix node + params)         ──┐
MM.2 (Custom UI — grid + offset row)            ──┼── ship
MM.3 (Tests + registry + showcase preset)       ──┘
```

Three small phases, ~2–3 days total. Ship as one PR.

### MM.1 — Core node (~half day)

`src/dsp/ModulationMatrix.h` — header-only, like `FOscillator` / `FChorus`.

```cpp
class FModulationMatrix : public TNodeBase<8, 8>
{
public:
    static constexpr uint32_t NumSources = 8;
    static constexpr uint32_t NumDestinations = 8;
    static constexpr uint32_t NumDepths = NumSources * NumDestinations;

    enum EParam : uint32_t
    {
        Param_DepthBase   = 0,
        Param_OffsetBase  = NumDepths,             // 64
        Param_COUNT       = NumDepths + NumDestinations,
    };

    // ... GetTypeName, GetInputPorts (8 Src), GetOutputPorts (8 Dst),
    // GetParamInfos (all hidden), Get/SetParamValue.

    void Process(const FProcessContext& Ctx) override
    {
        // Snapshot depths + offsets at block start so per-sample work
        // doesn't hammer atomics.
        float Depth[NumDestinations][NumSources];
        float Offset[NumDestinations];
        for (uint32_t I = 0; I < NumDestinations; ++I) {
            Offset[I] = OffsetParam[I].load(...);
            for (uint32_t J = 0; J < NumSources; ++J) {
                Depth[I][J] = DepthParam[I][J].load(...);
            }
            OutputSmoother[I].SetTarget(/* placeholder, set per-sample */);
        }

        const float* SrcBuf[NumSources];
        for (uint32_t J = 0; J < NumSources; ++J) {
            SrcBuf[J] = GetInputBuffer(J);
        }
        float* DstBuf[NumDestinations];
        for (uint32_t I = 0; I < NumDestinations; ++I) {
            DstBuf[I] = GetOutputBuffer(I);
        }

        for (uint32_t S = 0; S < Ctx.BlockSize; ++S) {
            for (uint32_t I = 0; I < NumDestinations; ++I) {
                float Sum = Offset[I];
                for (uint32_t J = 0; J < NumSources; ++J) {
                    if (SrcBuf[J] != nullptr) {
                        Sum += SrcBuf[J][S] * Depth[I][J];
                    }
                }
                OutputSmoother[I].SetTarget(Sum);
                DstBuf[I][S] = OutputSmoother[I].Tick();
            }
        }
    }

private:
    std::atomic<float> DepthParam[NumDestinations][NumSources];
    std::atomic<float> OffsetParam[NumDestinations];
    FOnePoleSmoother  OutputSmoother[NumDestinations];
};
```

Param param-info generation is mechanical — loop and emit 64 + 8 entries with names like `Depth_3_5` and `Offset_2`, all `bHidden = true`. Nothing else interesting at the param-layer level.

**Done when:**
- `FModulationMatrix` compiles, instantiates, and `GetInputPorts() / GetOutputPorts()` return 8 entries each.
- Manual test: connect `Constant(1.0) → Src0`, set `Depth_0_0 = 0.5`, observe `Dst0` outputs ~0.5 (post-smoother settle).

### MM.2 — Custom UI (~1 day)

`src/ui/ModMatrixUI.{h,cpp}`. Header declares `void DrawModMatrixUI(FModulationMatrix& Mat, FGraphModel& Sink);` (Sink for the slider-drag history coalescing pattern, mirroring `DrawSequencerUI`).

Render order:
1. Header label "Modulation Matrix"
2. Grid: Y axis = sources (rows 0..7), X axis = destinations (columns 0..7). Each cell uses `ImGui::PushID` and `ImGui::VSliderFloat` (vertical) sized ~36px × 24px. Tooltip on hover shows source/destination names + current depth value.
3. Offset row beneath the grid: 8 horizontal sliders, each ~40 px wide.
4. Right-click on any cell: popup with "Zero" + "Invert" entries.
5. Cells whose `Src_j` input port has no link wired get a dim foreground tint — the depth still works mathematically (the row is just zeros), but the visual cue makes "this depth does nothing" obvious.

Whether a port has a link wired requires reading the `FGraphModel`. The custom UI dispatcher already passes `Sink` (the model) to `DrawSequencerUI` for similar purposes, so the plumbing is in place.

**Done when:**
- Grid renders, mouse drag on a cell changes the underlying param.
- Right-click → Zero clears.
- Drag-then-release pushes one undo entry, not 100.
- Cell tint dims when the corresponding `Src` port is unwired.

### MM.3 — Tests + registry + preset (~half day)

`tests/ModulationMatrix.test.cpp`:

1. **Single route**: wire `Constant(1.0) → Src0`. Set `Depth_0_0 = 0.5`. After enough blocks for the smoother to settle, `Dst0[63]` is approximately `0.5`. Other outputs are zero.
2. **Sum**: wire `Constant(1.0) → Src0` and `Constant(2.0) → Src1`. `Depth_0_0 = 0.5`, `Depth_0_1 = 0.25`. Output 0 settles to `0.5*1 + 0.25*2 = 1.0`.
3. **Inversion**: `Depth = -1` produces `-input`.
4. **Offset**: with all sources disconnected, setting `Offset_0 = 0.7` yields `Dst0 ≈ 0.7` after smoothing.
5. **Disconnected source ignored**: connect `Src0` only. `Depth_0_5 = 0.9` (no source on `Src5`). `Dst0` doesn't pick up garbage from a disconnected port.
6. **Per-voice clone**: `Clone()` returns a non-null instance with identical depth/offset values. Setting per-voice flag in the model doesn't reject.
7. **Param round-trip via PatchSerializer**: save a model with non-zero depths, load, depths preserved.

Register `FModulationMatrix` in `NodeRegistry.cpp` under category **Modulation** with the description: *"Routes 8 CV sources to 8 CV destinations via per-cell depth knobs and per-output offsets. Replaces explicit LFO → Scale → Add chains."*

Add icon dispatch in `NodeIcons.cpp`. Glyph: 3×3 dot grid in `ColMath`, ~10 lines.

**Showcase preset**: `Lead/Matrix Lead`:
- Polyphonic core (Allocator → ADSR → Osc).
- One mono LFO at ~3 Hz, one mono LFO at ~7 Hz.
- Mod matrix routes:
  - LFO1 (3 Hz, sine) → Filter Cutoff @ +0.4 depth, offset 0.5 → SVF cutoff Control input.
  - LFO2 (7 Hz, triangle) → AutoPan rate @ +0.2 depth → AutoPan rate Control input. (Or any second destination that demonstrates the "one node fans out" win.)
- Two destinations of one matrix vs. four downstream Scale + Add nodes — concrete demonstration of the wire-count savings.

**Done when:**
- All tests pass.
- "Modulation Matrix" appears in the palette with the dot-grid icon.
- The showcase preset loads and produces audible filter modulation + auto-pan rate variation from a single matrix node.

---

## 3. Risks & mitigations

| Risk | Mitigation |
|---|---|
| 72 params clutter the patch JSON | They're hidden, so `Editor.cpp`'s standard widget loop skips them. JSON noise is identical to `FSequencer` (which has 64 hidden step params); accepted. |
| 8×8 grid is dense; tiny sliders are hard to drag | Right-click "Zero" + "Invert" reduces drag pressure. v2 could expose a "highlight rows where input is wired" filter to focus the active subset. |
| Per-output smoother chains a 30 ms tail onto every modulation change | Audible only at human-drag rate; a CV step from `0` to `1` settles in ~150 ms. Users editing depth in real-time hear a smooth fade rather than a click. Same convention as `FGain`. |
| Forgetting to set `bHidden = true` on all 72 params would surface a wall of sliders in the standard widget loop | Construct `GetParamInfos()` via a loop, set `bHidden = true` once at the top of the loop body. Compile fails fast in tests if this regresses (a "matrix has zero non-hidden params" assertion). |
| Connecting an Audio source to `Src0` is rejected by the type check, but the user might expect FM-like behaviour | Document in the registry tooltip: "Control inputs only. For audio-rate cross-mod, use a `Multiply` or `RingMod` node." |
| Per-voice matrix with mono sources accidentally routes garbage on unwired ports | Disconnected inputs read as `0` (the standard `GetInputBuffer` returns nullptr → branch in Process). Same convention every other node uses. |

---

## 4. What stays deferred

- **Per-source / per-destination labels.** v1 ports are `Src1..8` / `Dst1..8`. v2 could add a string param per port (`SrcLabel0..7`, `DstLabel0..7`) and surface them in the UI's row/column headers. Adds 16 params; only useful when patches grow past ~3 active routes.
- **Curve shaping per cell.** Each route is currently linear (`Depth × source`). Adding a per-cell curve choice (`Lin`, `Exp`, `Squared`, `S&H`) is a 16-state choice param + per-cell math; meaningful but bigger UI lift.
- **Bipolar / unipolar source modes.** A toggle that maps a `[0, 1]` ADSR into `[-1, +1]` before depth multiplication. Currently the user does this with a `Scale` node upstream — works fine.
- **> 8×8 size.** Compile-time sized; if there's demand, ship a `FModulationMatrix16x8` variant rather than parameterising. Avoids dynamic allocation in `Process`.
- **Audio-rate signal routing.** Ports are Control-only. Audio matrix is a separate problem (and a separate node).
- **Random / glide on depth changes.** Per-cell glide time would smooth manual tweaks more dramatically than the per-output smoother; nice-to-have.
- **Mod-source presets.** Bundled "starter" matrix configurations (e.g. "LFO + ADSR + key-track to filter, amp, pan"). Could ship as a few rows of seed-time depths, but the user can already set this up in seconds with the showcase preset as a template.
- **Visual feedback of live output values.** Metering the `Dst` outputs in real time would be useful but requires audio→UI ringbuffers per output (8 of them). Significant; defer.

---

## 5. Why this lands fast

The hard work is already in the engine:
- **Multi-input / multi-output base class** (`TNodeBase<8, 8>`) — already exists, used by `FVoiceAllocator` and `FSidPlayer`.
- **Hidden params + custom UI** — pattern already in production for `FSequencer`'s 64 step params and `FAdsr`'s envelope plot.
- **Slider-drag history coalescing** — handled centrally in `Editor.cpp`; new params inherit it for free.
- **Per-voice cloning** — default `Clone()` works because the matrix has only atomic-float state.
- **One-pole smoothers** — `FOnePoleSmoother` is the standard helper; 8 instances is trivial.

What's genuinely new is just the 8×8 ImGui grid layout and the sum-of-products inner loop. Both are mechanical.

Estimate: **~2–3 days end-to-end**, single PR, including tests + registry + the showcase preset.
