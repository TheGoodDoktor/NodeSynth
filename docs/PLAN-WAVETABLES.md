# NodeSynth — Wavetable Sources Plan

A new `FWavetableOscillator` node, sibling to `FOscillator` / `FSidPlayer`. A wavetable is a stack of single-cycle waveforms (frames); the synth interpolates between adjacent frames based on a `Position` param to produce evolving timbres. Industry standard since PPG Wave / Microwave; same design space as Serum, Massive, Vital, Ableton's Wavetable.

**Entry state:** Phase 5c done, Stage E.4 done, palette grouped by category, 28 bundled presets. `FOscillator` is the only periodic source (Sine/Saw/Square/Triangle/Noise with PolyBLEP). `FSidPlayer` already establishes the precedent for "node that loads a binary asset from disk on the UI thread and plays it on the audio thread" — read its `SetParamString` / async loader pattern before touching this.

**Goal:** drop a `FWavetableOscillator` into the graph, pick a wavetable from a bundled library, hear a clean (non-aliasing) tone whose timbre sweeps when the `Position` Control input is modulated by an ADSR / LFO. Per-voice cloneable so polyphonic patches work.

**Final deliverable that defines "done":**
- `FWavetableOscillator` registered in the palette under **Sources**.
- Bundles ship at least 4 wavetables (Basic Shapes, Vowels, FM Bell, PPG-style sweep).
- Position modulation produces audible smooth morph (no zipper, no clicks at frame boundaries).
- Aliasing harmonics suppressed below ~−60 dBFS at C7 on the brightest bundled wavetable.
- Per-voice flag works; 8-voice polyphonic patch using a wavetable runs at the same CPU as 8-voice `FOscillator` (within 2×).
- Two showcase presets land: one pad (slow position sweep), one lead (envelope-driven position).

---

## 1. Decisions to lock first

### 1.1 File format

**Decision: Serum-compatible single-WAV layout.** The `.wav` is read as one PCM stream; total length must be an integer multiple of the frame size. Frame size detected by trying common values (`2048, 1024, 512, 256`) and choosing the largest that divides cleanly.

| Frame size | Where it comes from | Status |
|---|---|---|
| 2048 | Serum / Vital / Ableton WT — the modern standard | Primary |
| 1024 | Older third-party WTs | Supported |
| 512  | Some hand-crafted single-cycle libraries | Supported |
| 256  | Microwave / PPG-era; legacy WT collections | Supported |

If detection fails (length isn't a multiple of any candidate), reject the file with a stderr warning. We deliberately *don't* support Serum's chunked metadata format (`clm`/`uhWT` chunks) in v1 — straight PCM only.

The internal canonical frame size is **2048** to keep the per-sample math uniform. Smaller-frame imports are **upsampled** to 2048 at load time using zero-stuffed FFT-domain insertion (zero-pad the spectrum, IFFT) — this preserves bandwidth without introducing the linear-interp lobing that a naive resample would.

Single-cycle WAVs (length == frame size) are treated as 1-frame wavetables. Position param has no effect; the WT plays as a fixed waveform.

### 1.2 Anti-aliasing strategy

**Decision: mip-mapped wavetables, FFT-built at load time.**

A 2048-sample frame contains up to 1024 harmonics. Played at C8 (~4.2 kHz fundamental, sample rate 48 kHz → Nyquist 24 kHz), only ~5 harmonics are legal; harmonics 6..1024 alias hard. We pre-compute **8 mip levels** per frame:

| Mip | Max harmonics | Audible up to | Cost |
|---|---|---|---|
| 0 | 1024 | C2 (~65 Hz) | full size |
| 1 | 512  | C3 (~131 Hz) | half |
| 2 | 256  | C4 (~262 Hz) | quarter |
| 3 | 128  | C5 (~523 Hz) | eighth |
| 4 | 64   | C6 (~1047 Hz) | … |
| 5 | 32   | C7 (~2093 Hz) | … |
| 6 | 16   | C8 (~4186 Hz) | … |
| 7 | 8    | high glitches | … |

Total memory cost ≈ 2× the original (geometric series 1 + 1/2 + 1/4 + … = 2). Acceptable for a typical 64-frame, 2048-sample wavetable: 64 × 2048 × 4 bytes × 2 = 1 MB per WT.

Build process at load:
1. For each frame, FFT (size 2048 → 1025 complex bins).
2. For mip `m`, zero out bins above `1024 / 2^m` then IFFT. Store as `float[2048]`. Keep the buffer at the same length — the harmonics are gone but the time-domain representation is at the original rate.

Per-block mip selection: pick `m = max(0, ceil(log2(fundamental / mip0_max_freq)))`. Clamped to `[0, 7]`. Selecting per *block* (not per sample) is fine — block size 64 is far below any audible mip-switch glitch frequency. Crossfade between adjacent mips for one block when the selection changes to suppress the boundary click. Simple linear crossfade in the buffer at the start of `Process()`.

We use `std::complex<float>` and a small radix-2 FFT (e.g. iterative Cooley-Tukey, ~80 lines) — no external FFT library dependency. The 2048-sample FFT at load time is cheap (~ms per frame, sub-second per WT). UI thread, never blocks audio.

### 1.3 Position modulation

**Decision: `Position` is a Float param 0..1 with a Control input, behaves like Oscillator's Frequency input (Control connection overrides param).**

When `Position * (NumFrames - 1)` lands between two integer frame indices, render via **linear interpolation** between the two frames' samples at the current phase. Linear is a deliberate v1 choice — Hermite/cubic between frames sounds marginally smoother on extreme position sweeps but doubles the per-sample cost (4 frame fetches instead of 2). Deferred. Linear-interpolated frame morphing is what most commercial wavetable synths actually do — it's not a corner-cut.

Position is **not** smoothed by `FOnePoleSmoother`. Reasoning: matches `FOscillator`'s Frequency / Amplitude treatment when a Control input is connected (see CLAUDE.md "Connected Control input overrides the param"). User-driven slider drags are smoothed via the existing UI-side coalescing path; modulation-driven changes are intentional and shouldn't be smeared.

### 1.4 Phase / playback math

Standard phase-accumulator approach (mirrors `FOscillator`):

```
PhaseInc = Frequency / SampleRate           // cycles per sample
Phase   += PhaseInc                         // wraps at 1.0
ReadIdx  = Phase * 2048                     // sample index within frame
                                            // (interpolated between [floor, ceil])
```

Sample read uses **2-point linear interpolation within frame** (not Hermite) — the FFT-built mips already have the high-freq content removed, so interp lobing is inaudible. Two adjacent frames combined with `Position` lerp gives 4 buffer reads + 3 multiplies per sample. CPU comparable to a polyBLEP saw.

### 1.5 Param surface

```
Wavetable   String  (file path, relative to wavetables/ directory)
Position    Float   0..1                     default 0.0   (Control input port)
Detune      Float   -100..+100 cents         default 0.0
Phase       Float   0..1 (initial offset)    default 0.0
Amplitude   Float   0..1                     default 1.0   (Control input port)
```

Five params total. `Wavetable` is a string — same `EParamKind::String` precedent set by `FSidPlayer`. The other four are floats; two of them (Position, Amplitude) get Control input ports.

**Frequency is a Control input only**, no param. Oscillator-style: when a Control source is wired (typically `VoiceAllocator.Frequency` or a `Sequencer.Frequency`), use it; otherwise the node is silent. Keeps per-voice patches simple (`Allocator → WT`) — no need to reach into a hidden default-440 param.

### 1.6 Threading & loading

Load happens **on the UI thread, in `SetParamString(Param_Wavetable, ...)`**. The same pattern `FSidPlayer` uses:

1. Open the WAV. Detect frame size. Reject on failure (log to stderr, leave previous WT loaded).
2. Allocate a `FWavetableData` (frames + mips) on the heap.
3. Run FFT mip generation on every frame.
4. Atomically swap `CurrentTable.store(NewTable)` (`std::atomic<std::shared_ptr<FWavetableData>>`).

Audio thread reads `CurrentTable.load(memory_order_relaxed)` once per `Process` call, holds a local `shared_ptr` for the block. Old `FWavetableData` is dropped from whichever thread releases the last `shared_ptr` — typically the UI thread on the next swap. Same RT-safe shared-pointer dance as `FAudioState::Graph`.

The `shared_ptr` deref + atomic load per Process call is negligible. **Never** allocate, FFT, or read from disk inside `Process`.

### 1.7 Library scanning

**Decision: identical convention to bundled presets.** Scan two roots:

- `<exe-dir>/wavetables/` — bundled, copied at build time via the same `add_custom_command(POST_BUILD)` pattern in `CMakeLists.txt` that already mirrors `presets/` next to the binary.
- `~/.nodesynth/wavetables/` — user-installed.

Both directories are walked recursively at startup, building a list of `(category, displayName, fullPath)` triples. Categories come from subdirectories: `wavetables/Basic/Saw.wav`, `wavetables/Vowels/AhEh.wav`, `wavetables/FM/BellRoyal.wav`. UI presents the list as a hierarchical dropdown in the property panel — exactly mirrors File → Presets.

Bundled set for v1 (small but representative):
- `Basic/Sine.wav`, `Basic/Saw.wav`, `Basic/Square.wav`, `Basic/Triangle.wav` (single-frame, redundant with `FOscillator` but useful for showing the system works).
- `BasicShapes.wav` — 4-frame morph between Sine → Triangle → Saw → Square. Shows position sweep on familiar timbres.
- `Vowels.wav` — 5-frame Ah / Eh / Ee / Oh / Oo formant sweep.
- `FMBell.wav` — 32-frame, generated programmatically (see §1.8) showing harmonic morph.
- `PPGSweep.wav` — 64-frame, programmatic, classic "saw → square via comb" morph.

### 1.8 Generated bundled wavetables

The four "interesting" bundled WTs are **generated by a Catch2 emit-test**, same pattern as `EmitPresets.test.cpp`. Tag `[.][wavetable-emit]`. Hidden by default; runs `./build/Release/nodesynth_tests.exe "[wavetable-emit]"` to regenerate the canonical files. Output goes to `wavetables/<Category>/<Name>.wav`.

This keeps the wavetable content in the repo as both the canonical `.wav` (for users) and the generation script (for reproducibility). Bundling raw `.wav` files would be opaque; checking in only the script would mean every build runs the generator. Both is the right call — same trade-off the preset bundle made.

### 1.9 Per-voice cloning

**Decision: override `Clone()` to share the underlying `FWavetableData` pointer.**

The default `INode::Clone()` does a fresh-instance + param-copy by name. For a wavetable node this would re-trigger a full WAV load + FFT mip rebuild on every per-voice clone — fatal. Override:

```cpp
std::shared_ptr<INode> FWavetableOscillator::Clone() const override
{
    auto C = std::make_shared<FWavetableOscillator>();
    C->CurrentTable.store(CurrentTable.load());     // share data
    C->WavetablePath = WavetablePath;               // string param
    C->Position.store(Position.load());
    // ... copy remaining atomic params
    return C;
}
```

Tested directly: 8-voice clone path stays fast (no disk I/O, no FFT) and all voices read from one shared `FWavetableData`.

### 1.10 Position fan-out at high modulation rates

A worry: connecting an LFO at 20 Hz to `Position` makes the synth read from rapidly-changing pairs of frames. On a 64-frame WT, that's a pair of frames swept ~1280 times per second — within harmonic territory. Does this introduce its own aliasing?

**Answer: yes, but bounded and acceptable.** The morphing operation is a per-sample `lerp(frameA, frameB, alpha)` with `alpha` slewed at the LFO rate. For a 20 Hz LFO sweeping the full range, the morph spectrum has energy up to ~20 Hz × NumFrames ≈ 1.3 kHz — well below Nyquist, and the frames are themselves bandlimited. So no inter-frame aliasing for any reasonable LFO rate.

If a user wires an audio-rate Control source (e.g. `Multiply` of two LFOs) into Position, they'll get something resembling FM. We don't try to defend against that — it's a creative use, not a bug.

---

## 2. Sub-phases

```
WT.1 (data class + WAV loader)             ──┐
WT.2 (FWavetableOscillator, no AA)         ──┤
WT.3 (mip generation + selection)          ──┼── ship
WT.4 (Position CV + Amplitude CV)          ──┤
WT.5 (UI: dropdown + frame preview)        ──┤
WT.6 (per-voice clone override)            ──┤
WT.7 (bundled WTs + showcase presets)      ──┘
```

The first four are the substantive work. Each phase ships independently — graph compiles + tests pass at the end of each.

### WT.1 — Data class + WAV loader (~1 day)

`src/dsp/Wavetable.h` — new header. No node yet; just the shared data plumbing.

```cpp
struct FWavetableMip
{
    std::vector<float> Samples;          // one frame's worth, 2048 long
};

struct FWavetableFrame
{
    std::array<FWavetableMip, 8> Mips;   // mip 0 = full bandwidth
};

struct FWavetableData
{
    std::vector<FWavetableFrame> Frames;
    static constexpr uint32_t FrameSize = 2048;
    static constexpr uint32_t NumMips = 8;
};

// Load + build mips. UI thread. Returns nullptr on failure.
std::shared_ptr<FWavetableData> LoadWavetable(const std::filesystem::path& Path);
```

WAV reading is uncompressed PCM only — 16-bit / 24-bit / 32-bit-int / 32-bit-float, mono, any sample rate. Standard parser (~150 lines, no external dep). Stereo input is summed to mono; we don't currently have a use case for stereo wavetables, and most authoring tools export mono.

FFT: small radix-2 iterative Cooley-Tukey in a separate `src/dsp/internal/Fft.h` (private — wavetables are the only consumer for now; if Phase 7 needs it for spectrum analysis we promote it).

**Done when:**
- `tests/Wavetable.test.cpp`: load a known WAV, verify frame count, verify mip 0 sample-by-sample equals the original (with the FFT round-trip tolerance).
- Mip 7 of a saw frame contains only the lowest 8 harmonics (assert via FFT of mip 7 — energy above bin 8 should be ~0 within float epsilon).

### WT.2 — Bare oscillator node (~half day)

`src/dsp/WavetableOscillator.h` — header-only, like `FOscillator`. `TNodeBase<3, 1>` (Frequency, Position, Amplitude inputs; one Audio output). No mip selection yet — always plays mip 0.

Phase accumulator + linear frame interp + linear sample interp, exactly as §1.4. Position from param only (no Control input wired yet).

`Wavetable` param is `EParamKind::String`. `SetParamString` triggers a `LoadWavetable` and atomic-swaps the held `shared_ptr<FWavetableData>`.

Register in `NodeRegistry.cpp` under category **Sources** with a placeholder description. Add a default icon (just the existing sine glyph from `FOscillator` in `ColSource` orange — drawing a "stack of waveforms" glyph for the WT can come in WT.5).

**Done when:**
- Manually setting `Wavetable` to a single-frame `Saw.wav` produces a 220 Hz saw indistinguishable from `FOscillator` Saw at the same pitch (under audible AA on low notes — high notes will alias, expected).
- Loading a 4-frame `BasicShapes.wav` and sweeping `Position` 0 → 1 morphs Sine → Triangle → Saw → Square at 220 Hz.

### WT.3 — Anti-aliasing mips (~1.5 days)

Add mip generation to `LoadWavetable`. Mip selection in `Process`:

```cpp
const float Fundamental = FreqIn[0];
const float Mip0Cutoff = SampleRate / 2.0f / FrameSize * 1024.0f;  // ~Nyquist for mip 0
const int32_t Mip = static_cast<int32_t>(std::max(0.0f,
    std::ceil(std::log2(std::max(Fundamental / Mip0Cutoff, 1.0f)))));
const int32_t MipClamped = std::clamp(Mip, 0, 7);
```

(The math simplifies to: pick the lowest mip whose harmonic count keeps `harmonics × fundamental ≤ Nyquist`. The per-block compute is ~4 ns. Negligible.)

Cross-fade for one block on mip change: render once with the previous mip and once with the new mip, lerp from 0→1 across the block, sum. Approx 1.5× cost on the transition block; back to 1× immediately after. The `LastMip` per-channel state is one int.

**Done when:**
- A WT containing a 1024-harmonic sawtooth, played at C7 (~2 kHz), produces output where any FFT bin above 24 kHz is < −60 dBFS. (Test uses an offline FFT to measure spectrum.)
- Audible: high notes are clean, no metallic alias buzz, on a 64-frame programmatic FM bell wavetable.

### WT.4 — Position + Amplitude Control inputs (~half day)

Add Control input ports for Position (port 1) and Amplitude (port 2). `Process` reads the buffer-or-param pattern from `FOscillator`:

```cpp
const float* PosBuf = GetInputBuffer(Input_Position);
for (uint32_t I = 0; I < BlockSize; ++I) {
    const float P = (PosBuf != nullptr) ? PosBuf[I] : PositionParam;
    ...
}
```

Per-sample mod path with Control connected; per-block param path otherwise. Same structure as `FOscillator::Process`.

**Done when:**
- ADSR → Position produces audible morph through frames over the envelope's lifetime.
- LFO → Position produces a smooth periodic morph at LFO rate.
- Test: connect Position to a `Constant(0.5)` and assert output identical (to float epsilon) to setting `Position` param to `0.5` with no input wired.

### WT.5 — UI: dropdown + frame preview (~1 day)

Property panel custom UI for `FWavetableOscillator`, dispatched from `DrawPropertyPanel` (same pattern as `AdsrUI`, `SequencerUI`, `MeterUI`).

Two parts:

1. **Picker.** Hierarchical dropdown built from the library scan. Tree by category subdirectory; click a leaf to set `Wavetable` param via the dual-write path (direct `SetParamString` + queued `SetParam` command — see CLAUDE.md "dual write" pattern). On selection, the audio thread sees the new WT on the next block.

2. **Frame preview.** ImGui `PlotLines` showing the current frame's mip 0 samples. Updates when `Position` changes — we sample which integer frame is closest to `Position * (N-1)` and draw it. ~50 lines. Optional v2: 3D view (Serum-style) showing all frames stacked. Out of scope for v1.

The WT library is scanned **once at startup** and cached. Not auto-refreshed — adding a WT mid-session requires a restart. (Same as presets currently.) A "Rescan" button in the menu can land in v2 if it bites.

**Done when:**
- Property panel shows a dropdown that lists every bundled WT.
- Selecting one swaps the audio cleanly with no clicks (one-block crossfade as the new mip-0 frame replaces the old).
- Preview redraws in real-time as `Position` is dragged.

### WT.6 — Per-voice clone override (~half day)

Implement `Clone()` per §1.9. Test: 8-voice polyphonic patch with a wavetable oscillator marked per-voice. Hold 8 keys; assert no audible difference from a mono patch at a higher master gain (the mip-0 buffers should be byte-identical across voices).

CPU sanity: 8 voices × wavetable should be no more than 2× the cost of 8 voices × `FOscillator` Saw at the same pitch. Rough microbenchmark in the test (run for N blocks, compare wall time).

**Done when:**
- The per-voice flag is accepted (`SetNodePerVoice` doesn't reject) on `FWavetableOscillator`.
- A polyphonic wavetable patch loads, plays 8-voice chords, and the per-voice mixer correctly sums the clones.

### WT.7 — Bundled wavetables + showcase presets (~half day)

Add `EmitWavetables.test.cpp` — generates the four programmatic WTs (`BasicShapes`, `Vowels`, `FMBell`, `PPGSweep`) and writes them as PCM WAVs into `wavetables/`. Bundle them into the build via the existing CMake POST_BUILD pattern.

Two showcase presets in `EmitPresets.test.cpp`:
- `Pad/Wavetable Drift`: WT pad with a slow LFO sweeping Position 0→1 over ~8 s. Shows the gradual morph.
- `Lead/Wavetable Lead`: WT lead with ADSR → Position so each note articulates through the table.

**Done when:**
- `wavetables/` directory is committed to git with the four bundled WAVs.
- Both showcase presets load and play.

---

## 3. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Big WT (256 frames × 2048 × 8 mips) takes 16 MB resident | Cap loaded WTs at 256 frames at parse time; reject larger files with a warning. Bundled set stays under 64 frames per WT. |
| FFT mip generation slow on cold load | One 2048-FFT runs in well under 1 ms on a modern CPU; even a 256-frame WT × 8 mips = 2048 FFTs ≈ 100 ms. UI thread; user perceives a brief stall on first load. Acceptable. Cache by content hash later if it bites. |
| `shared_ptr` swap during a per-voice clone broadcast race | The audio-thread drain happens before voice processing each block, and clones share via `atomic<shared_ptr>`. Test directly: swap the WT mid-block on a polyphonic voice and assert no torn-buffer crash. |
| Mip selection switches every block when fundamental crosses an octave boundary while sweeping pitch via portamento | The crossfade-on-change masks the discontinuity. Worst case is ~5 ms of double-cost rendering — inaudible. |
| Wavetable load fails silently (file missing, format unsupported) | `SetParamString` returns false; node keeps the previously loaded WT. UI surfaces the error inline in the property panel. Same UX as `FSidPlayer`. |
| Linear frame interp produces audible "stair" artifacts when Position hits exact integer values | Linear-interp at integer frame index: `lerp(frameA, frameB, 0.0)` is exactly `frameA`. No stair — just a single frame. The artifact some authors warn about is from *cubic* interp overshooting; we use linear so it's fine. |
| WAV files in different bit depths / sample rates produce different perceived loudness | Normalize peak amplitude to 1.0 at load time. Ship a `Detune` and let the user trim with a downstream `Gain`; we don't expose a pre-norm gain — no value over what's already there. |
| Aliasing test is flaky because the FFT bin where energy lands depends on phase | Use a long enough block (≥ 8192 samples) and average across multiple windows. Test framework patterns from the existing `Equalizer.test.cpp` continuous-phase sine generator apply here. |
| Per-voice clone shares mutable state if I forget the atomic on `CurrentTable` | The `CurrentTable` member must be `std::atomic<std::shared_ptr<FWavetableData>>` not just `std::shared_ptr`. Static analyser + Catch2 multi-thread test enforces this. |

---

## 4. What stays deferred

- **Spectral / harmonic editing.** Serum's standout feature is editing the frames as spectra. Big UI lift; v1 is a player only.
- **In-app wavetable creation.** No "import from audio" or "draw a frame". Authoring happens externally.
- **Stereo wavetables.** Internal data is mono. A stereo node could land later; needs IsOutputStereo plus per-channel mip arrays (~doubles the memory).
- **Hermite / cubic frame interpolation.** Linear is fine. Revisit if a user hears artifacts.
- **Position smoothing on slider drag.** The UI's `IsItemActivated` / `IsItemDeactivatedAfterEdit` coalescing path already collapses drags to one history entry. Param itself isn't smoothed — see §1.3.
- **WAV chunk metadata (Serum `clm` / `uhWT`).** Not part of the plain PCM contract. Files authored with these chunks still load — we just ignore the metadata.
- **Resampling on import.** All input WAVs are assumed at the project sample rate. We don't currently resample anything else either; consistent.
- **3D wavetable visualisation.** Stack-of-frames view is nice but takes a real ImGui DrawList layout pass. v2.
- **FM-WT hybrid.** Routing one WT's audio output as another's Frequency CV (audio-rate FM) is creative but type-mismatch with the current Audio/Control split. Cross-cuts the engine.
- **WT spectrum tilt / formant shift.** Single-knob spectral filter applied per-frame at load time. Adds substantial param surface; better as a follow-on plugin node (e.g. `FSpectralTilt` between WT and downstream).
- **Auto-rescan on directory change.** File watcher complexity; users can restart for now.

---

## 5. Why this is bigger than Phaser

Phaser was 2 days because the DSP, UI, and bundled examples were all small. Wavetables touch:

- A new **non-trivial data structure** (mipped frame stack) that needs to allocate, FFT, atomic-swap, and clone safely.
- A new **file format parser** (WAV) — well-trodden, but ~150 lines we didn't have.
- A new **library-scan + UI dropdown** pattern (the synth has presets, not WT picker; some plumbing to share).
- **Shared loaded data** across per-voice clones — a clone-override is needed.
- A new **bundled-asset emit pipeline** (analogous to preset emit, separate test and CMake hook).

Realistic estimate: **~1 to 1.5 weeks** of focused work for v1. Phase split lets each WT.X land green individually.

The architecture is well-precedented: `FSidPlayer` already does async asset loading + atomic swap + UI thread parsing, presets already establish the bundled-files-near-binary convention, and `FOscillator` already establishes the phase-accumulator + Control-input-overrides-param pattern. Mostly it's stitching existing patterns together with one new piece of DSP (the mip generator) on top.
