# NodeSynth — Effects Roadmap

A staged development plan covering 14 effect nodes, prioritised by **value per effort**. Each stage ships independently; pick where to stop.

**Entry state:** Phase 5c complete. Effect nodes already in: `FDelay`, `FReverb`, `FWaveshaper`, `FChorus`, `FFlanger`, `FPhaser`. The pattern for stereo effects (header-only `TNodeBase<1, 1>`, `IsOutputStereo(0)` returning true, smoothed float params, NodeRegistry entry) is well-established.

**Goal:** plug the obvious gaps in the effect roster (dynamics, EQ, tremolo) before circling back to the niche / experimental ones (granular pitch shift, multi-band compression, stutter).

**Final deliverable that defines "done":** all of E.1–E.4 lands. E.5 is optional / on-demand.

---

## 1. Decisions to lock first

### 1.1 Param conventions for dynamics

**Decision: thresholds in dB, ratios as `:1`, attack / release in ms.** Match every plugin the user has ever seen. Stick with `Param_ThresholdDb` (-60..0), `Param_Ratio` (1..20), `Param_AttackMs` (0.1..200), `Param_ReleaseMs` (5..2000), `Param_MakeupGainDb` (0..24).

Detector type: peak-following one-pole on `|x|`. RMS is a v2 toggle. The audible difference between the two is small at musical settings, and peak-detection is the simpler implementation.

### 1.2 EQ topology

**Decision: cascaded RBJ biquads.** Three filters in series for the 3-band EQ — low-shelf, peak, high-shelf. RBJ cookbook coefficient formulas (well-known, robust, MIT-licensed-ish public-domain math). Same biquad math as `FSvf` but with different coefficient calculations per filter type.

State variables: 4 floats per biquad (z1, z2 per channel). For stereo: doubled state, single coefficient calc per param change.

### 1.3 Detector sharing across compressor / limiter / gate

**Decision: factor the envelope detector into an inline helper** (`FEnvelopeFollower` in `src/dsp/Envelope.h`), used by all three. One implementation, three different gain-curve mappings.

Limiter is a compressor variant (ratio = ∞, hard knee, fast attack). Gate is a compressor variant (curve below threshold = reduce by inverse ratio). All three share the detector, vary the curve.

### 1.4 Stereo dynamics behaviour

**Decision: linked stereo detection.** Detector reads `max(|L|, |R|)`, single gain value applied identically to both channels. Prevents stereo image collapse when one channel is louder than the other. Standard for hardware compressors. v2 could expose an "unlinked" toggle for special cases.

### 1.5 Limiter lookahead

**Decision: defer.** Real lookahead requires latency reporting across the graph (so downstream effects align). NodeSynth has no latency-compensation concept today. v1 limiter is a fast-attack hard-knee compressor with no lookahead — adequate for synth use, won't catch transients perfectly. A proper lookahead limiter is a Stage E.5+ concern.

### 1.6 Bitcrusher topology

**Decision: sample-and-hold downsample + bit-depth quantize.** Two parameters: `Param_SampleRate` (host SR / 64 .. host SR) and `Param_Bits` (1..16). S&H rate is integer division of host rate. Quantize is `floor(x * 2^bits) / 2^bits`. Stateless except for a per-channel sample counter. ~20 lines.

Alternatives like dithered quantize, soft S&H interpolation — defer.

### 1.7 Ring modulator

**Decision: dedicated `FRingMod` node** even though the user could chain `FOscillator → FMultiply → FInput`. Reasons: (a) one-node UX matches Reverb / Delay / Chorus etc., (b) the dedicated node smooths the carrier-frequency param (a Multiply with a Constant doesn't), (c) a "Mix" param to blend dry + ring is convenient.

Carrier built into the node (Sine / Triangle / Square Choice). External carrier via Control input is a v2 enhancement.

### 1.8 Tremolo / Auto-pan as separate nodes vs one combined

**Decision: separate.** They share zero implementation (one modulates amplitude, one modulates pan), and the UX is clearer with two single-purpose nodes than one with a "Mode" toggle. Both small enough that duplication is fine.

### 1.9 EQ band count for v1

**Decision: 3-band fixed (low-shelf, peak, high-shelf).** Most synth patches need EQ for tone-shaping rather than surgical correction. A configurable N-band parametric EQ is a v2 follow-up.

### 1.10 Effects deferred from this plan entirely

These don't fit the "easy DSP" bucket and have their own architectural cost:
- **Convolution reverb** — IR loading, FFT-based convolution. Multi-week project.
- **PSOLA / phase-vocoder pitch shifting** — quality pitch shift is genuinely hard.
- **Look-ahead limiting** — needs graph-wide latency compensation (see §1.5).
- **External clock sync** for tempo-synced effects — see PLAN-PHASE-5 §5 deferred list.

---

## 2. Stages

```
E.1 Dynamics (Compressor + Limiter + Gate)        ──┐  highest value-per-effort
E.2 Tone shaping (EQ + DC blocker)                ──┤
E.3 Modulation completes (Tremolo + Auto-pan)     ──┤  ship as the "fill the obvious gaps" milestone
E.4 Character / utility (Bitcrusher + Ring mod    ──┤
     + Stereo widener + Haas + Exciter)           ──┘
E.5 Exotic (Multi-band compressor + Granular pitch ── optional, gate on demand
     shift + Stutter / beat-repeat)
```

E.1 alone makes the synth feel notably more capable. E.2–E.4 round out the effect roster to industry-standard. E.5 is for users who'd specifically benefit.

### Stage E.1 — Dynamics (~3 days)

**E.1.1 Envelope detector helper (~½ day)**
- `src/dsp/Envelope.h`: header-only `FEnvelopeFollower` with `Prepare(Sr)`, `Reset()`, `Process(float Input, float AttackMs, float ReleaseMs) → float`. Peak-following one-pole on `|x|`, asymmetric attack / release coefficients per call (so live param changes track).
- Tests: DC step at +0 dBFS reaches ≥0.95 in `5 × AttackMs` worth of samples; releases below 0.05 in `5 × ReleaseMs` after input drops to zero.

**E.1.2 `FCompressor` (~1 day)**
- Stereo, linked detection: detector reads `max(|InL[i]|, |InR[i]|)`, computes gain reduction from the over-threshold amount × (1 - 1/Ratio), applies the same gain to L and R.
- Params: `Threshold (dB)`, `Ratio (1..20)`, `AttackMs`, `ReleaseMs`, `MakeupGainDb`.
- Tests: a sine at -6 dBFS through a 4:1 compressor with -12 dB threshold gets reduced by ~4.5 dB; below threshold passes through unchanged; stereo image preserved (L/R reduction equal to within `1e-5`).

**E.1.3 `FLimiter` (~½ day)**
- Same skeleton as compressor with `Ratio` hardcoded to ∞ (hard ceiling), `Threshold` becomes `CeilingDb` (default 0). Attack/release tighter (default 1ms / 100ms). No lookahead — documented limitation.
- Params: `CeilingDb`, `ReleaseMs`, `MakeupGainDb`. Attack hidden (always fast).
- Tests: a +6 dBFS impulse output peaks at ≤ Ceiling + 0.5 dB; sustained signal below ceiling passes through.

**E.1.4 `FGate` (~½ day)**
- Same detector as compressor; gain curve below threshold reduces by `(threshold - input) × (1 - 1/Ratio)` instead of above. Hold time so brief drops below threshold don't cut the signal.
- Params: `ThresholdDb`, `Ratio` (high default for gating, e.g. 10:1), `AttackMs`, `HoldMs`, `ReleaseMs`.
- Tests: input below threshold reduced by expected amount; input above passes through; hold prevents fluttering on a signal that hovers around the threshold.

**Done when:** all three nodes registered, audible compression / limiting / gating on a synth chain, 215+10 = 225 tests passing.

### Stage E.2 — Tone shaping (~1.5 days)

**E.2.1 RBJ biquad helper (~½ day)**
- `src/dsp/Biquad.h`: header-only `FBiquad` with `SetLowShelf / SetPeak / SetHighShelf / SetLowPass / SetHighPass / SetBandPass` per RBJ cookbook formulas, `Process(float) → float` direct-form-II transposed.
- Tests: low-shelf at unity gain is bit-identical to passthrough; peak filter at +6 dB at fc shows expected boost in spectrum (FFT not required — single sine at fc, measure RMS).

**E.2.2 `FEqualizer` (~½ day)**
- Stereo, three biquads in series per channel.
- Params: `LowShelfFreqHz`, `LowShelfGainDb`, `PeakFreqHz`, `PeakGainDb`, `PeakQ`, `HighShelfFreqHz`, `HighShelfGainDb`. Seven params — wide but each is intuitive.
- Tests: all gains at 0 dB → bit-identical passthrough; LS gain +6 at 200 Hz reduces 50 Hz amplitude less than 1 kHz amplitude; HS gain +6 at 8 kHz boosts 12 kHz more than 1 kHz.

**E.2.3 `FDcBlocker` (~½ day)**
- Stereo, single-pole highpass at fixed 20 Hz. No params (or just an enable toggle).
- Tests: DC input → output decays to zero within ~50 ms; 1 kHz sine through unchanged within passband flatness tolerance.

**Done when:** EQ + DC blocker registered, audible tone-shape effects on the seeded patch.

### Stage E.3 — Modulation completes (~1 day)

**E.3.1 `FTremolo` (~½ day)**
- Stereo, single LFO modulating amplitude. Same convention as Chorus / Flanger / Phaser.
- Params: `RateHz` (0.05..20, log), `Depth` (0..1), `Shape` (Choice: Sine / Triangle / Square / Saw), `Stereo` (Choice: Mono / Quad — quad puts L/R 90° out of phase).
- Tests: depth 0 → bit-identical passthrough; depth 1 → output ranges from 0 to input level once per LFO period; stereo mode produces L/R divergence.

**E.3.2 `FAutoPan` (~½ day)**
- Stereo, LFO modulating L/R balance (constant-power pan). Mono input becomes panned-stereo output.
- Params: `RateHz`, `Depth`, `Shape`. Always stereo.
- Tests: depth 0 → output equal on L and R (centred); depth 1 → output alternates fully L / fully R; constant-power: L² + R² ≈ Input² always.

**Done when:** the modulation-effects quartet (Chorus / Flanger / Phaser / Tremolo) is complete; auto-pan rounds out the stereo positioning.

### Stage E.4 — Character / utility (~2.5 days)

**E.4.1 `FBitcrusher` (~½ day)**
- Stereo, sample-and-hold downsample + quantize.
- Params: `SampleRateRatio` (0.01..1), `Bits` (1..16), `Mix`.
- Tests: ratio 1.0 + bits 16 → effectively bypass (within float precision); ratio 0.1 → output stays constant for ≥10 host samples between updates; bits 1 → output is one of {-1, +1} per sample.

**E.4.2 `FRingMod` (~½ day)**
- Stereo, internal sine carrier multiplied with input. Simple but smoothed.
- Params: `CarrierHz` (1..5000, log), `Shape` (Choice: Sine / Triangle / Square), `Mix`.
- Tests: at carrier 0 Hz mix 1 → output stays at the input × constant; carrier 100 Hz on a 1 kHz input produces sum/difference frequencies (verified by RMS difference vs dry input over a long window).

**E.4.3 `FStereoWidener` (~½ day)**
- Mid-side processing: M = (L+R)/2, S = (L-R)/2; multiply S by `Width` (0..2, default 1); reconstruct L/R = M ± S.
- Params: just `Width`. One-line DSP.
- Tests: width 1 → bit-identical passthrough; width 0 → L == R (mono); width 2 → louder side content.

**E.4.4 `FHaasWidener` (~½ day)**
- Add a fixed 5–25 ms delay on one channel (or both, asymmetric). Gives mono input an apparent stereo width via the precedence effect.
- Params: `DelayMs` (5..30), `Side` (Choice: Right / Left), `Mix`.
- Tests: delay 0 → output equal on L and R; delay 15 ms → R lags L by 720 samples at 48k.

**E.4.5 `FExciter` (~½ day)**
- Pre-highpass (~3 kHz) → soft saturation (tanh) → mix back with dry. Adds high-frequency harmonics for "presence".
- Params: `Frequency` (1..10 kHz, the HP corner), `Drive` (0..40 dB), `Mix`.
- Tests: drive 0 → bit-identical passthrough; drive high → output has more high-frequency energy than input.

**Done when:** five small character effects round out the roster.

### Stage E.5 — Exotic (~1+ weeks each, optional)

**E.5.1 Multi-band compressor (~3 days)**
- Linkwitz-Riley 4th-order crossovers split into 2 bands (low/high) — keep it 2-band for v1 to avoid the phase-coherence headache that 3+ bands introduce. Each band gets its own threshold / ratio / attack / release. Recombine summing.
- Risk: Linkwitz-Riley pairs are phase-coherent at the crossover but only at one frequency; linear-phase crossovers exist but are FIR and add latency.
- Skipping for v1 unless requested.

**E.5.2 Granular pitch shifter (~3 days)**
- Buffered audio + overlapping windowed grains played back at variable rate. ~50–100 ms grain length, 50 % overlap, Hanning window. Quality is "recognisable, not transparent" — better than nothing, worse than reSampler / Antares.
- Risk: smooth pitch modulation produces audible grain-boundary artefacts. Acceptable for synth use.

**E.5.3 Stutter / beat-repeat (~3 days)**
- Audio-rate buffered slicing tied to a Clock node. On each clock pulse, latch the next N samples; play them back repeatedly until the next pulse.
- Risk: tied to the still-deferred external-clock-sync feature for full musical use. Without it, just a fixed-rate stutter.

---

## 3. Sequencing & budget

```
Day 1-3:    E.1 Dynamics (Compressor + Limiter + Gate)
Day 4-5:    E.2 Tone shaping (EQ + DC blocker)
Day 6:      E.3 Modulation completes (Tremolo + Auto-pan)
Day 7-9:    E.4 Character / utility (Bitcrusher + Ring mod + Widener + Haas + Exciter)
[Day 10+:   E.5 Exotic, on demand]
```

Total without E.5: **~9 focused days**. Each individual node is small (½–1 day); the time mostly goes into the test scaffold + parameter UX polish per node.

If you want to bail early, **E.1 alone** delivers most of the value. E.2 plus E.1 covers what 90% of synth patches actually want from effects.

---

## 4. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Compressor / limiter without lookahead misses very fast transients | Document. v1 is for synth use where transients are usually predictable from the envelope. Add lookahead in E.5+ once the latency-compensation infrastructure exists. |
| RBJ biquad coefficients can blow up at very low Q (<0.5) or near Nyquist | Clamp `Q` to `[0.1, 10]` and frequencies to `[20, 0.45 × Sr]` per `SetParamValue`. Same belt-and-braces as `FSvf`. |
| Bitcrusher S&H at very low ratios produces audible aliasing tones | Aliasing IS the bitcrusher effect — that's what users want. No mitigation needed; document. |
| Stereo widener at width >> 1 produces L = -R (anti-phase mono content cancels out on a mono sum) | Document. Users who care about mono compatibility set width ≤ 1.5. v2 enhancement: a "mono compat" indicator. |
| 7 params on the EQ node makes the property panel feel cluttered | Group into named subsections in the property panel via custom UI (same hook pattern as `DrawAdsrUI` etc.). Default property panel still works. |
| Haas widener's fixed delay sounds wrong on certain content (drums) | Acceptable for synth — Haas is a synth/pad effect, not a drum effect. Document. |
| Auto-pan with constant-power maths produces a slight loudness pumping at the centre | Negligible at typical depths. If users complain, add a "linear pan" toggle. |
| `FEnvelopeFollower` shared header-only across three nodes — changes ripple | Only one implementation, deliberate. The signature is small and stable; changes to it are intentional. Tests on the helper itself catch regressions. |

---

## 5. What stays deferred

Even after E.1–E.4 ship, these remain on the parking lot per `docs/PLAN-PHASE-5.md` and `docs/PLAN.md`:

- **Convolution reverb** — IR loading, FFT convolution. Different ballgame.
- **Plate / hall reverb algorithms** — Freeverb covers v1.
- **Linear-phase crossovers** for multi-band. Adds latency that the graph can't compensate for yet.
- **Lookahead limiter** with proper transient catching.
- **PSOLA / phase-vocoder pitch shifting**.
- **Tap-tempo on `FDelay` / `FChorus` / `FFlanger` / `FPhaser` / etc.** — wants Clock-node integration, see external-clock-sync.
- **External clock sync** — MIDI clock, Ableton Link, ReWire.
- **Tempo-aware modulation rates** — same dependency.
- **FFT-based effects** generally — spectral gate, vocoder, pitch correction. Each is its own meaningful project.
- **Aliasing-correct distortion / saturation variants** beyond the existing `FWaveshaper` opt-in.

---

## 6. Decision checklist before starting E.1

A few things flagged above that warrant explicit confirmation before code:

1. **Peak vs RMS detector** — recommend peak; RMS as a v2 toggle. (§1.1)
2. **Linked vs unlinked stereo detection** for compressor — recommend linked. (§1.4)
3. **No lookahead in v1 limiter** — recommend deferring. (§1.5)
4. **One detector helper shared across compressor / limiter / gate** — recommend yes. (§1.3)
5. **3-band EQ fixed**, not configurable N-band — recommend yes. (§1.9)

If any of these go differently, the per-stage sub-phases shift slightly but the overall ordering stands.
