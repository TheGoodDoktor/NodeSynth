# NodeSynth — Vocoder Plan

A **channel vocoder**: a `FVocoder` node that imposes the moving spectral envelope of a *modulator* signal (classically a voice) onto a harmonically-rich *carrier* (classically a saw/square synth), producing the "talking synth" effect. Plus a companion **`FMicInput`** live-capture node so the modulator can be your actual voice in real time.

**Entry state:** Backlog empty. Subgraphs shipped on `feature/subgraphs`. Stereo signal path live; `FEnvelopeFollower` (dynamics) and `FBiquadCoeffs`/`FBiquadState` (EQ) primitives exist and are reused here. Audio device is **playback-only** (`ma_device_type_playback` in `main.cpp`); there is no audio-capture or file-input path anywhere in the codebase yet.

**Goal:** speak into a microphone, hear a synth pad "say" the words. Drop a `Mic Input` → `Vocoder.Modulator`, an `Oscillator` (saw) → `Vocoder.Carrier`, `Vocoder` → `Output`, and get intelligible robot-choir vocals with adjustable band count, attack/release, and formant shift.

**Final deliverable that defines "done":** both nodes registered in the palette; live mic vocodes a saw carrier with recognisable speech intelligibility at 16 bands; silence on the modulator yields silence out (gated by the envelope followers); zero allocation in either `Process`; tests green; bundled `Vocoder Talk` demo preset.

---

## 0. Why this is two nodes, not one

A vocoder is only as interesting as its modulator. The carrier is trivial — any oscillator. The modulator is the hard half, and NodeSynth has **no live-audio or file input** today; every source (`Oscillator`, `Wavetable`, `SID`, noise) is synthetic. Vocoding a synth with another synth gives rhythmic/robotic texture but never *speech*. So the real "talking synth" needs a capture path.

**Decision (confirmed with the user): build a live `FMicInput` capture node alongside `FVocoder`.** The mic node is the larger engineering lift (a second audio device + lock-free hand-off to the audio thread); the vocoder DSP itself is mostly bookkeeping over primitives we already have.

The two are independently shippable and independently testable. `FVocoder` can be developed and unit-tested first using a synthetic modulator (white noise, an oscillator); `FMicInput` slots in as the modulator source once it lands.

---

## 1. `FVocoder` — decisions to lock first

### 1.1 Topology

**Decision: classic channel vocoder.** Per band `i`:

```
modulator ──▶ analysis BPF_i ──▶ envelope follower_i ──┐
                                                        ▼  (× per-sample gain)
carrier  ───▶ synthesis BPF_i ─────────────────────────●──▶ Σ bands ──▶ out
```

The modulator is split into N bands; each band's amplitude envelope is tracked. The carrier is split into the *same* N bands; each carrier band is scaled by its matching modulator envelope; the scaled bands are summed. Where the modulator has energy in band *i*, the carrier passes in band *i* — so the carrier "wears" the modulator's spectrum.

### 1.2 Band-pass primitive

**Decision: 4-pole band-pass = two cascaded RBJ biquads sharing one `FBiquadCoeffs`.** Reuse the existing `FBiquadState` (Direct-form I, already tested) and add one method:

```cpp
// FBiquadCoeffs (src/dsp/Biquad.h) — RBJ constant 0 dB peak-gain BPF.
void SetBandpass(float Fc, float Q, float SampleRate)
{
    const float W0    = 2.0f * 3.14159265358979f * Fc / SampleRate;
    const float CosW  = std::cos(W0);
    const float Alpha = std::sin(W0) / (2.0f * Q);
    Normalise(Alpha, 0.0f, -Alpha,          // b0, b1, b2  (0 dB peak gain)
              1.0f + Alpha, -2.0f * CosW, 1.0f - Alpha);  // a0, a1, a2
}
```

One coeff set per band per role (analysis / synthesis), two `FBiquadState` in series for a 4-pole / 12 dB/oct skirt — enough band separation to keep speech intelligible without the CPU of higher orders. A single 2-pole biquad (6 dB/oct) leaks too much between adjacent bands and smears consonants; we cascade two.

*(Alternative considered: TPT/ZDF band-pass lifted from `FSvf`. Rejected for v1 — the RBJ biquad path reuses an already-tested primitive verbatim and constant-Q is trivial to express. TPT is the v2 move if we ever want per-sample cutoff modulation of the bands, which a vocoder doesn't need.)*

### 1.3 Band count & spacing

**Decision: `Choice` param `Bands` = 8 / 16 / 24, default 16. Log-spaced centres, 120 Hz … min(8 kHz, 0.45·SR).** Arrays sized to a fixed `MaxBands = 24` — no runtime allocation when the count changes.

- **8 bands** — coarse, "robot", strong musical pitch, low intelligibility. Cheap.
- **16 bands** — the classic (Roland VP-330 / EMS territory). Good intelligibility/character balance. Default.
- **24 bands** — most intelligible, smoother, closest to the source vowels.

Centres are geometric: `Fc_i = Fmin · (Fmax/Fmin)^(i/(N-1))`. Constant-Q: `Q = Fc_i / Bandwidth_i`, with `Bandwidth_i` set from the ratio between adjacent centres so bands tile the spectrum with roughly -3 dB crossovers. Computed once in `Prepare` and whenever `Bands` changes (detected at block start, like `FChorus`'s voice-count change).

### 1.4 Envelope follower

**Decision: reuse `FEnvelopeFollower` (`src/dsp/Envelope.h`), one instance per band.** It already does asymmetric attack/release peak-following with per-block `SetTimes`. Expose:

```
Attack   Float (ms, log)   0.5 .. 50    default 5      // fast → crisp consonants
Release  Float (ms, log)   5   .. 500   default 40     // slow → smeared, "choir"
```

Fast attack + moderate release is the intelligible sweet spot; long release gives the lush pad-vocoder sound. These two params are the main expressive control after band count.

### 1.5 Modulator is mono; carrier (and output) is stereo

**Decision: detect the envelope from a mono sum of the modulator (L+R)·0.5; run two synthesis banks (L, R) on the carrier.** Voices are mono in practice, and a single envelope set keeps the band gains identical across channels so the stereo image of the *carrier* is preserved. Cost: analysis bank ×1 (mono), synthesis bank ×2 (stereo).

`IsOutputStereo(0)` returns `true`. A mono carrier fans its L into both synthesis banks (per existing broadcast plumbing) and the output is dual-mono; a stereo carrier stays stereo.

### 1.6 Formant shift

**Decision: `Formant` Float, 0.5 … 2.0, default 1.0.** The synthesis bank centres are the analysis centres × `Formant`. At 1.0 they coincide (true vocoder). >1 shifts the carrier's spectral peaks up ("chipmunk"), <1 down ("monster"). This is a cheap, high-value creative knob — the analysis envelopes are unchanged, only the carrier filters retune. Synthesis centres clamp to `[20 Hz, 0.45·SR]`.

### 1.7 Dry/wet and output trim

```
Mix      Float (0..1)        default 1.0    // 0 = dry carrier passthrough, 1 = fully vocoded
Output   Float (dB, -24..24) default 0      // smoothed makeup gain; vocoding loses level
```

`Mix` blends the raw carrier against the vocoded sum (per channel). Vocoding attenuates substantially (the carrier is gated by per-band envelopes that are rarely all near 1), so `Output` makeup gain is genuinely needed — smoothed via `FOnePoleSmoother` like every other gain in the codebase.

### 1.8 Param surface (final)

```
Bands     Choice  8 / 16 / 24    default 16
Attack    Float   0.5 .. 50 ms   default 5     log
Release   Float   5 .. 500 ms    default 40    log
Formant   Float   0.5 .. 2.0     default 1.0
Mix       Float   0 .. 1         default 1.0
Output    Float   -24 .. 24 dB   default 0
```

Ports: **In 0 = Carrier (Audio)**, **In 1 = Modulator (Audio)**, **Out 0 = Audio**. (Two audio inputs is already supported — `FMixer` has four.)

### 1.9 Per-voice

**Decision: leave mono (master-bus effect).** It's cloneable via the default `Clone`, but per-voice makes no musical sense (one carrier, one mic). No special handling needed; document that the per-voice flag, while not rejected, has no useful effect.

### 1.10 Deferred (v2)

- **Unvoiced/sibilance path** — a high-pass bleed of the modulator (or a noise burst keyed by a broadband HF detector) mixed in to restore the "s/t/sh" consonants that band-limited vocoders swallow. Genuinely improves intelligibility; non-trivial; explicitly v2.
- **Adjustable Q / band overlap** param.
- **Freeze / hold** (latch the current envelope set — frozen-vowel pads).
- **Per-band gain editing** (a graphic-EQ-style custom UI over the band envelopes).

---

## 2. `FMicInput` — decisions to lock first

### 2.1 Device strategy

**Decision: a separate `ma_device` of type capture, owned by the node, mirroring the `FMidiInput` ownership pattern — NOT a duplex device.**

The existing playback device in `main.cpp` stays exactly as-is. `FMicInput` owns:
- its own `ma_context` (for device enumeration in the property panel), and
- a capture `ma_device` whose data callback runs on miniaudio's capture thread and writes mono samples into a lock-free SPSC ring.

`Process` drains the ring into the node's output buffer at block start. This is the *same shape* as `FMidiInput` → `FMidiRing` (callback thread writes, `Process` drains), so it composes with the two-thread model without touching the snapshot-swap or the playback callback.

*Rejected: duplex device.* Replacing the playback device with a duplex one would entangle capture into the main audio callback and the snapshot lifecycle, and forces capture+playback onto one backend/clock. The separate-device path is more code-isolated and matches an existing, proven pattern. The cost is two independent device clocks (see 2.4).

### 2.2 Capture format & ring

- Open capture at the graph sample rate (`FAudioState::SampleRate`), **mono** (`ma_device_config.capture.channels = 1`), `ma_format_f32`. miniaudio resamples/downmixes from the hardware device as needed.
- `FMicRing` — a 4096-sample (≈85 ms @ 48 k) lock-free SPSC float ring under `src/audio/` (new directory; sibling to `src/midi/`). Capture callback is producer; `Process` is consumer.
- `Process`: drain up to `BlockSize` samples into output channel 0; broadcast to channel 1 via the standard mono convention. On underrun (ring has < BlockSize), emit what's there and zero-fill the rest. On overrun (UI not draining — shouldn't happen, audio thread always drains), the producer drops oldest.

### 2.3 Params & lifecycle

```
Device   Choice   (enumerated capture devices; index 0 = "Default")
Gain     Float    -24 .. 24 dB    default 0     // smoothed input trim
```

- `Device` enumerated via `ma_context_get_devices` when the property panel opens (UI thread).
- Open/close the capture device on the **UI thread** from `SetParamValue(Param_Device, …)` — same threading rule as `FMidiInput`'s device open. Never from the audio callback.
- **Non-cloneable:** override `Clone()` → `nullptr` (owns a device, UI-stateful). Per-voice flag rejected by `SetNodePerVoice`, exactly like `FMidiInput` / `FVirtualKeyboard` / `FOutput`.

### 2.4 Known caveats (document, don't solve in v1)

- **Clock drift:** capture and playback are independent device clocks. Over minutes the ring fill level drifts; the 85 ms ring plus drain-what's-there absorbs it with occasional single-block glitches. Sample-rate-tracking resampling is a v2 concern.
- **Feedback howl:** mic → vocoder → speakers → mic. Document "use headphones" prominently in the node tooltip and the demo preset notes.
- **Destruction blocks briefly:** `ma_device_uninit` joins the capture thread — same caveat already documented for `FMidiInput`'s `RtMidiIn` teardown. New-graph publishes run on the UI thread, so old graphs holding the mic node are typically released on the UI thread; an audio-thread release could cause a momentary dropout. Acceptable for v1.
- **Latency:** capture period + ring + block ≈ low tens of ms. Fine for a vocoder; not for monitoring yourself dry.

---

## 3. Sub-phases

```
V.1  FBiquadCoeffs::SetBandpass + test                ──┐
V.2  FVocoder node (synthetic modulator) + tests      ──┼── vocoder shippable on its own
V.3  FMicInput node (capture device + ring) + tests   ──┤
V.4  Registry + icons + demo preset + docs            ──┘── "talking synth" end-to-end
```

V.1→V.2 is the vocoder; V.3 is the mic; V.4 wires them up. V.2 and V.3 are independent and could land in either order (or parallel branches).

### V.1 — Band-pass primitive (~half day)

Add `SetBandpass` to `FBiquadCoeffs` (§1.2). `tests/Vocoder.test.cpp` (or extend `Equalizer.test.cpp`):
- Unity-ish peak gain at `Fc` for a sine swept to centre; strong attenuation an octave away.
- Two cascaded states give a steeper (4-pole / 12 dB/oct) skirt than one (measure rolloff at 2× and 0.5× Fc).
- DC and Nyquist → near-zero output.

### V.2 — `FVocoder` node (~1.5 days)

`src/dsp/Vocoder.h` (header-only, like the other effects). `TNodeBase<2, 1>`. Fixed arrays `[MaxBands]` of: analysis coeff/state×2, `FEnvelopeFollower`, synthesis coeff + state×2 (×2 for L/R). `Prepare` computes centres + Qs; `Process` detects band-count/formant changes, recomputes coeffs when they change, then runs the per-sample loop (§1.1). Params per §1.8, gains smoothed.

Tests (using white-noise / oscillator modulator — no mic needed):
1. **Silence gate:** modulator silent → output silent (envelopes decay to ~0), regardless of carrier energy.
2. **Spectral tracking:** modulator = sine at band *k*'s centre, carrier = white noise → output energy concentrates in band *k* (FFT or band-power check). Proves the analysis→synthesis routing.
3. **Mix = 0** → output equals carrier (dry passthrough, within float tolerance).
4. **Formant shift:** modulator sine at band *k*, `Formant = 2.0`, carrier = white noise → output energy peak moves up ≈ one octave.
5. **All three band counts** produce non-silent output for noise modulator + noise carrier (guards against an all-bands-bypassed bug).
6. `IsOutputStereo(0)` is true; **no allocation in `Process`** (pointer held across many blocks).
7. `Clone()` round-trips all six params.

### V.3 — `FMicInput` node (~2 days)

`src/audio/MicRing.h` (SPSC float ring; model on `src/midi/MidiRing`). `src/dsp/MicInput.{h,cpp}` (`.cpp` because it owns miniaudio objects and does device enumeration — keep miniaudio include out of the widely-included headers). `TNodeBase<0, 1>`.

- Capture device init/uninit on UI thread via `SetParamValue(Param_Device, …)`; callback writes mono f32 → ring; `Process` drains (§2.2).
- Enumerate devices for the `Device` Choice (UI thread).
- `Clone()` → `nullptr`; rejected by `SetNodePerVoice`.

Tests (no real hardware in CI):
1. `FMicRing` push/drain under a concurrent producer/consumer thread pair (mirror `MidiRing.test.cpp`) — no loss/dup at the SPSC boundary.
2. `FMicInput::Process` with a hand-filled ring → output channel 0 matches pushed samples, channel 1 broadcasts.
3. Underrun (empty ring) → zero-filled block, no crash.
4. `Clone()` returns `nullptr`; `GetTypeName()` stable.

*No CI test opens a real capture device* — device I/O is manual-verified. Note this explicitly in the test file so the coverage gap is visible.

### V.4 — Registry, icons, preset, docs (~half day)

- Register both nodes in `NodeRegistry.cpp`: `FMicInput` under **Sources** (next to `SidPlayer`), `FVocoder` under **Effects** (next to `RingMod` — same spectral-character family). Include `Mic Input` / `Vocoder` menu labels + tooltips (mic tooltip carries the headphone-feedback warning).
- `NodeIcons.cpp`: reuse the source/effect accent colours; a custom glyph is optional for v1 (a mic glyph for the input, a vertical-bars "filter bank" glyph for the vocoder would be nice-to-have).
- **`Vocoder Talk` demo preset:** `Mic Input → Vocoder.Modulator`, sawtooth `Oscillator` (rich harmonics) `→ Vocoder.Carrier`, `Vocoder → Gain(makeup) → Output`. Carrier oscillator can be played from the on-screen keyboard so you choose the "voice" pitch while you talk. Preset notes mention headphones.
- Docs: flip the "backlog empty" lines in `CLAUDE.md`/`docs/BACKLOG.md`, add the two nodes to the node-set inventory in `CLAUDE.md`, note the new `src/audio/` directory.

---

## 4. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Poor speech intelligibility | 16-band default with 4-pole skirts is the proven baseline; fast attack (5 ms) preserves consonants. Sibilance path is the documented v2 lever if it's not enough. |
| Vocoded output too quiet | `Output` makeup-gain param (§1.7); the demo preset ships with a sensible default. |
| Band-count change clicks | Recompute coeffs on change and accept one boundary block of discontinuity — same approach as `FChorus`'s voice-count change. Document. |
| RBJ BPF unstable near Nyquist | Clamp every band centre (analysis and formant-shifted synthesis) to `[20 Hz, 0.45·SR]`, like `FSvf`. |
| Mic feedback howl | Headphone warning in tooltip + preset notes; `Mix`/`Output` start conservative. Not a code problem. |
| Capture/playback clock drift | 85 ms ring + drain-what's-there tolerates it with rare single-block glitches; SR-tracking resample deferred to v2. |
| Capture device unavailable / permission denied (OS mic privacy) | `ma_device_init` failure path: log to stderr, leave the node emitting silence, surface "no device" in the property panel. Never crash. |
| miniaudio capture API differs across backends (WASAPI/CoreAudio/ALSA) | miniaudio abstracts this; we already trust it for playback. Test manually on the two CI platforms (Windows/macOS) before merge. |
| CI can't test real capture | SPSC ring + `Process` drain are unit-tested with a synthetic producer; device open is manual-verified and flagged in the test file. |

---

## 5. Effort & sequencing

- **V.1** ~0.5 day — one method + tests.
- **V.2** ~1.5 days — the vocoder DSP; reuses `FEnvelopeFollower` + biquads, so it's mostly the band-bank bookkeeping and tests.
- **V.3** ~2 days — the real work: new capture device, ring, device enumeration, UI-thread lifecycle, cross-platform check.
- **V.4** ~0.5 day — wiring, preset, docs.

**Total ≈ 4.5 days.** The vocoder alone (V.1+V.2+partial V.4) is ~2.5 days and shippable with a synthetic modulator if you want to validate the DSP before committing to the capture path.

Suggested branch: `feature/vocoder` off `main` (or off `feature/subgraphs` once that merges). Per the project's working style: build + test + commit at each sub-phase boundary.
