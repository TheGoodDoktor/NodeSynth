# NodeSynth — Phaser Plan

A new `FPhaser` node, sibling to `FChorus` / `FFlanger`. Phasers create their characteristic sweeping notches by cascading **all-pass filters** whose corner frequencies are modulated by an LFO — *not* by modulating delays (that's flanger territory). Different DSP, different sound.

**Entry state:** Phase 5c complete. Stereo signal path live, `FChorus` + `FFlanger` shipped, `IsOutputStereo` plumbing in place.

**Goal:** drop a Phaser node into the graph, get a sweep that ranges from MXR Phase 90-style subtle to deep negative-feedback Univibe-y territory, with stereo width by default. Same UX as Chorus / Flanger.

**Final deliverable that defines "done":** `FPhaser` registered in the palette, audible sweep across all four shape choices (4/6/8 stages, with/without feedback), stereo divergence test passes (L and R diverge under identical mono input), zero allocation in `Process`.

---

## 1. Decisions to lock first

### 1.1 All-pass topology

**Decision: 1st-order all-pass, tan-based coefficient.**

Per-stage transfer function `H(z) = (a + z⁻¹) / (1 + a·z⁻¹)`, with `a = (1 - tan(π·fc/fs)) / (1 + tan(π·fc/fs))` where `fc` is the per-stage corner frequency. Standard textbook design (e.g. Pirkle, Reiss/McPherson). At `fc`, the all-pass introduces 90° phase shift; cascading N gives N·90° at corner, which produces N/2 notches when summed with the dry signal.

This is one multiply + one add per sample per stage. CPU is negligible — 8 stages × 2 channels × per-sample = 16 mac/sample, dwarfed by Reverb / Chorus.

### 1.2 Number of stages

**Decision: Choice param `Stages` with values `4 / 6 / 8`.**

- **4 stages** — subtle, 2 notches. Small Stone vibe.
- **6 stages** — Phase 90 / classic guitar phaser. Default.
- **8 stages** — denser, more vocal-y.

User-selectable rather than fixed because they're audibly distinct and each suits a different patch.

### 1.3 LFO modulation curve

**Decision: exponential (log-frequency).** Each stage's corner sweeps as `fc = BaseFreq · 2^(Depth · LFO)`, where `LFO ∈ [-1, 1]`.

`BaseFreq` is a fixed 800 Hz centre — chosen because the most musical phaser sweeps span roughly 200 Hz to 3 kHz, and 800 Hz is the geometric mean. `Depth` then expands or contracts that range:

- `Depth = 0` → no modulation (static all-pass, mostly inaudible).
- `Depth = 1` → ±2 octaves (200 Hz ↔ 3.2 kHz).
- `Depth ∈ [0..1]` → smaller sweep ranges.

Linear-frequency modulation would sound bottom-heavy (too much time at low frequencies, where notches are perceptually compressed). Exponential matches how the ear hears pitch — same as octave-up / octave-down on a slider.

### 1.4 Feedback

**Decision: signed `[-0.95, +0.95]`, like `FFlanger`.**

Last all-pass output taps back into the cascade input scaled by `Feedback`. Positive feedback emphasises the notches' troughs; negative feedback (alternation) gives the Phase 90's sharper "vocal" sound. Cap at ±0.95 to prevent runaway in the unlikely failure cases.

Default `0.5` — audibly phaser-y without being hyperactive.

### 1.5 Stereo

**Decision: two parallel cascades, L/R LFO 90° out of phase.** Same convention as `FChorus` / `FFlanger`. Mono input fans out to both channels; stereo input is processed channel-paired.

`IsOutputStereo(0)` returns `true` so the graph plumbs L→L, R→R into the consumer.

### 1.6 Per-stage corner offsets

**Decision: identical per stage (all stages sweep together) for v1.**

The "MXR Phase 100" approach gives each stage (or pair of stages) its own LFO with phase offsets, producing a richer sound at the cost of a clear notch pattern. v1 keeps it simple — all stages share the same `fc` per channel. v2 enhancement could expose a per-stage-offset spread.

### 1.7 Param surface

```
Rate      Float (Hz, log)  0.05 .. 5.0   default 0.4
Depth     Float (0..1)                   default 0.7
Feedback  Float            -0.95..0.95   default 0.5
Mix       Float (0..1)                   default 0.5
Stages    Choice           4 / 6 / 8     default 6
```

Same five-param shape as `FFlanger` plus the `Stages` choice. Keeps the per-effect UX consistent.

---

## 2. Sub-phases

```
P.1 (FAllpass1stOrder + per-channel cascade) ──┐
P.2 (FPhaser node)                            ──┼── ship
P.3 (tests + registry)                        ──┘
```

Small enough that this could land in one PR; broken out for readability.

### P.1 — All-pass primitive (~half day)

Inline `FAllpass1stOrder` struct in `src/dsp/Phaser.h` (no separate file — it's an internal detail of the phaser, not a reusable public DSP component yet):

```cpp
struct FAllpass1stOrder
{
    float Z = 0.0f;                 // single delay element
    float Process(float X, float A) // A = filter coefficient
    {
        const float Y = -A * X + Z;
        Z = X + A * Y;
        return Y;
    }
    void Reset() { Z = 0.0f; }
};
```

Plus a coefficient helper:

```cpp
static float AllpassCoeff(float Fc, float Sr)
{
    const float T = std::tan(static_cast<float>(M_PI) * Fc / Sr);
    return (1.0f - T) / (1.0f + T);
}
```

**Done when:** unit test confirms a unity-gain DC pass-through and a 90° phase shift at the chosen `Fc`.

### P.2 — `FPhaser` node (~1 day)

`src/dsp/Phaser.h` (header-only, like `FChorus` / `FFlanger`):

- `TNodeBase<1, 1>` — one Audio in, one Audio out.
- Two cascades of `MaxStages = 8` `FAllpass1stOrder` instances (L and R). Cascade length used = `4 / 6 / 8` per the `Stages` choice; unused stages are bypassed (Process them with `A = 0` would still mutate state — better to skip the call entirely).
- `LfoPhase` accumulator (double, wraps at 2π). Per sample, advance by `2π · Rate / SampleRate`.
- L LFO = `sin(LfoPhase)`, R LFO = `sin(LfoPhase + π/2)`.
- Per sample per channel: compute `Fc = 800 · 2^(Depth · LFO)`, derive `A = AllpassCoeff(Fc, SR)`, run input + (FeedbackTap × Feedback) through the cascade, store the cascade output as the new `FeedbackTap`. Mix wet + dry.
- `IsOutputStereo(0)` returns true.
- All params smoothed via `FOnePoleSmoother` at 30 ms TC (matches Chorus / Flanger).

Same `FLine`-style internal struct can be lifted from `FFlanger` (just rename and replace the delay-line buffer with the all-pass cascade). Or just inline both channels directly — only 2 of them.

**Done when:** the node compiles, registers, runs a 1 kHz sine through and produces audible sweep at default settings.

### P.3 — Tests + Registry (~half day)

`tests/Phaser.test.cpp`:

1. `IsOutputStereo(0)` returns true.
2. `Mix = 0` → output equals input on both channels (dry passthrough).
3. **Stereo divergence**: same as the Chorus / Flanger tests — feed identical mono input on L and R, run for ~100 blocks, assert L and R outputs accumulate non-trivial difference (proves the 90° L/R LFO offset is wired).
4. **Feedback decay**: `Mix = 1`, impulse on input, run for 50 blocks of silence — total output energy must be non-zero (the feedback path keeps the impulse alive briefly).
5. **All four stage counts** (`Stages = 4 / 6 / 8`) all produce non-silent output for a sine input — guards against accidentally bypassing all stages.

Register `FPhaser` in `NodeRegistry.cpp`:

```cpp
{ "Phaser", "Phaser",
  "Stereo all-pass-cascade phaser. 4/6/8 stages, signed feedback,\n"
  "exponential LFO modulation. Different from chorus / flanger:\n"
  "modulates filter phase, not delay time.",
  []() -> std::shared_ptr<INode> { return std::make_shared<FPhaser>(); },
},
```

Add to `NodeIcons` per-type dispatch (just reuse `ColEffect` purple — same accent as Chorus / Flanger / Reverb / Delay / Waveshaper). Custom glyph optional; the default node icon works fine for v1.

**Done when:** "Phaser" appears in the palette, the seeded patch gets a Phaser between Gain and Output, and the user hears the classic notch sweep.

---

## 3. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Feedback near ±1 causes instability or DC offset accumulation | Cap at ±0.95 in `SetParamValue`. Same belt-and-braces as `FFlanger`. |
| Stages-choice change while audio is playing produces a click | Smooth the stage transition by running the previous N stages for one block, then the new N for subsequent blocks — accept the boundary discontinuity. Same shape as `FChorus`'s Voices change. v1: just accept the click; document. |
| Tan-based coefficient blows up near `Fc → SR/2` | Clamp `Fc` to `[20, SR · 0.45]` per sample — same clamp `FSvf` uses on its cutoff. |
| Per-sample `pow(2, x)` for the LFO modulation curve is expensive | Negligible at 8 stages × 2 channels (the dominant cost is the all-pass cascade itself). If profiling later shows it, switch to `std::ldexp` or a polynomial approximation. v1: don't optimise prematurely. |
| Static configuration (no modulation) sounds dead | At `Depth = 0` the phaser is just a static all-pass — phase shifts the dry signal but the spectrum is unchanged so nothing audible happens. Document. The user wants `Depth ≥ 0.1` for any audible effect. |

---

## 4. What stays deferred

- **Per-stage LFO offsets** (MXR Phase 100 / Univibe-style staggered modulation). Adds richness; not v1.
- **Higher-order all-pass filters** (2nd-order biquad all-pass for steeper notches per stage). Marginal.
- **Tap-tempo on Rate**. Same deferral as Chorus / Flanger / Delay.
- **External LFO sync** (Clock node drives Rate). Bigger architectural change shared with the other modulation effects.
- **Sample-and-hold LFO** option (square / S&H instead of sine). Easy to add later as a `LfoShape` Choice param.
- **Wet mono / dry stereo**. Currently both are stereo; a "preserve dry stereo image" toggle would matter for stereo input chains.

---

## 5. Why this lands fast

Phaser DSP is **simple** — ~30 lines for the core, no buffer allocation, no delay-line indexing. It's mostly bookkeeping (the cascade loop, the LFO accumulator, the param smoothing). The plumbing patterns (stereo `IsOutputStereo`, `FOnePoleSmoother` per param, Choice param for stage count, `NodeRegistry` entry, divergence test) all exist verbatim in `FFlanger` and `FChorus` — copy-paste-modify is genuinely the right approach.

Estimate: **~2 days end-to-end**, including tests and registry wiring. Could be done in a single sitting if the user is happy to skip the polish step (e.g. dedicated icon glyph, demo preset).
