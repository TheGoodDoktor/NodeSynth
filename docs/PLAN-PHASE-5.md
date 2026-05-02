# NodeSynth — Phase 5 Plan

The "real synth" phase. Stereo lands, presets land, the DSP gains polish (oversampling on the nonlinear nodes, dedicated chorus/flanger), and — optionally and gated on whether you want NodeSynth in a DAW — a VST3 wrapper.

**Entry state:** Phases 0–4.5 complete. 181 Catch2 tests / 64k+ assertions green. Synth is feature-complete in mono: polyphonic subtractive engine, 25+ nodes, undo/redo with composite history, native file dialog, patch metadata, portamento, proactive link validation. The compile-error UX, last-good snapshot, and per-voice partition algorithm all carry forward unchanged.

**Phase 5 goal:** NodeSynth sounds like a real synth. Stereo width, anti-aliased waveshapers, ready-to-load presets, and modulation effects (chorus/flanger) that demand stereo to be worthwhile. After Phase 5 the synth is in the same league as a basic commercial soft-synth — VST3 then turns it into one a user can actually drop into a DAW session.

**Final deliverable that defines "done":** all of 5a + 5b + 5c land cleanly with tests; 5d ships only if you decide you want VST3 hosting. The deferred list at the end stays for Phase 6+ (or never).

---

## 1. Decisions to lock first

### 1.1 Stereo representation

**Decision:** widen `Audio`-typed buffers from `float[BlockSize]` to `float[2][BlockSize]` (interleaved-by-channel, contiguous L then R). `EPortType::Audio` *is* stereo; there is no separate `Stereo` port type.

**Why this over alternatives:**
- *Paired L/R Audio ports* — doubles the port count on every node, ugly UI, error-prone wiring. Rejected.
- *New `EPortType::Stereo`* — splits the type system, every node has to declare which it is, every patch needs validation rules. Rejected.
- *Interleaved samples (`LRLRLR…`)* — matches miniaudio's device callback layout but breaks SIMD-friendly per-channel processing inside nodes. Rejected.

**Mono nodes** write the same value to both channels (a one-line broadcast in `Process`). Most existing nodes (`FOscillator`, `FAdsr`, `FSvf`, `FGain`, `FVca`, math nodes) start mono — they'll be migrated to "naturally mono, broadcast on output" first; "true stereo" implementations come later as needed.

**`Control`-typed buffers stay mono.** A control voltage is a scalar; widening it to stereo solves no real problem. Modulation that wants stereo behaviour (e.g. a stereo LFO) gets two Control outputs, not one stereo output.

**Audio thread implication:** every `Process` body that writes to an output buffer becomes a 2-channel write. Block-buffer storage in `TNodeBase` doubles in size (still allocation-free at runtime). The miniaudio callback already runs at the device's channel count — current code in `main.cpp` writes mono to both channels; the change is to read `[0]` for L and `[1]` for R from the sink's buffer.

### 1.2 Stereo migration order

**Big-bang vs gradual.** Big-bang (one PR migrates every node) is cleaner but a multi-day non-shippable branch. Gradual (introduce stereo buffers behind a feature flag, migrate nodes one at a time) is messier but always-shippable.

**Decision:** big-bang. The `BlockSize` width change is a single edit to `TNodeBase`'s storage; every node gets one mechanical update (broadcast scalar mono into both channels in `Process`). Estimated 1–2 days of mostly-mechanical work. A feature flag would carry permanent two-codepaths complexity for no upside — once stereo lands, mono is *gone*.

Test coverage protects this: every existing test asserts on `OutputBuffer[0]`. After migration, those become "channel L" assertions, and we add a few "L == R" assertions for the broadcast invariant on mono nodes. Anything that breaks gets caught immediately.

### 1.3 Stereo-aware nodes (post-migration)

After the broadcast-everywhere migration, these nodes get *true* stereo behaviour:
- **`FOutput`** — already a sink; just stops broadcasting and plumbs `[0]` to L, `[1]` to R for the device callback.
- **`FGain`** / **`FVca`** — scalar gain applies equally to both channels (no behaviour change). Adds a Pan param later (a Phase 6 nice-to-have, not 5).
- **`FDelay`** — two independent delay lines (one per channel). Adds a "Stereo Spread" param (offset L vs R delay times) later.
- **`FReverb`** — Freeverb is *natively* stereo; the current implementation downmixes to mono. Switch to true stereo. Free win.
- **Chorus / Flanger (new)** — phase-offset LFOs on L vs R for stereo width.

**`FWaveshaper`, `FOscillator`, `FAdsr`, `FSvf`, math nodes, sequencer, scope, meter** stay mono-broadcast. No reason to compute the same thing twice.

### 1.4 Preset format & layout

**Format:** unchanged — presets are regular `.json` patches, schema v1, identical to user-saved files. No separate format, no preset-specific fields.

**Layout:**
```
presets/
  init/
    Init Patch.json
  bass/
    Sub Bass.json
    Acid Lead.json
  pads/
    Soft Strings.json
    ...
```

Bundled into the install directory at build time (CMake `install` rule copies `presets/` next to the binary). On startup, scan the install dir's `presets/` and the user's `~/.nodesynth/user_presets/` (if present) and merge.

**UI:** new "Preset" submenu in the File menu; nested by directory. Selecting a preset is just `LoadPatch` on the chosen file. The current `Open…` dialog stays for ad-hoc files.

**Initial preset count:** 8–12, hand-tuned. Categories: Init / Bass / Lead / Pad / FX. Keep them small and listenable; one per category covers the basics.

### 1.5 Oversampling architecture

**Decision:** per-node opt-in via a `Param_Oversample` Choice param (1x / 2x / 4x). Default 1x to keep CPU cost zero unless the user asks for it. Implementation: each node that opts in owns an `FOversampler` (new utility class in `src/dsp/Oversampler.h`).

**`FOversampler`** topology: polyphase half-band FIR (order ~31 taps, configurable). Two filters total — one upsampler (`InRate → 2*InRate`), one downsampler (`2*InRate → InRate`). Wraps the node's "true" `Process` in upsample → process at 2x → downsample. Applied recursively for 4x.

**Nodes that get the param in 5c:**
- **`FWaveshaper`** — biggest aliasing offender; the entire point of this feature.
- **`FOscillator`** Square / Saw at high frequencies — PolyBLEP already mitigates aliasing but oversampling on top is worth offering for users who want it.

Other nonlinear nodes (`FSvf` self-oscillation, `FAdsr` exponential curves) stay 1x — their nonlinearities are gentle enough that aliasing isn't audible at typical settings.

**Block-size implication:** at 2x the internal block becomes 128 samples; at 4x, 256. Buffers in `FOversampler` are pre-allocated in `Prepare`. Process is allocation-free.

### 1.6 Chorus & Flanger node design

Both are "modulated short delay" effects — same DSP backbone, different param ranges and a feedback path on the flanger.

**`FChorus`:**
- Stereo: two delay lines (L/R), LFO phase offset 90° between them for width.
- Params: Rate (0.05..10 Hz), Depth (0..1, scales delay-time modulation), Mix (dry/wet), Voices (Choice: 1 / 2 / 3 — number of stacked LFO-offset taps per channel).
- Delay range: 5..30 ms typical. No feedback (chorus is feedback-free by definition; user wanting feedback wants flanger).

**`FFlanger`:**
- Stereo: two delay lines (L/R), LFO phase offset 90°.
- Params: Rate (0.05..5 Hz), Depth (0..1), Feedback (-0.95..0.95, signed — negative inverts), Mix.
- Delay range: 0.5..10 ms — much shorter than chorus, the comb-filter signature.
- Feedback sign matters: positive emphasises odd harmonics, negative emphasises even.

Both can today be patched as `LFO → FDelay.Time + Mix` (the Delay node's Time control input is intentionally unsmoothed precisely so an LFO produces clean modulation). The dedicated nodes ship sensible defaults and the flanger's feedback path, which is awkward to wire as a graph cycle.

### 1.7 VST3 wrapper (5d) — scope & decision gate

VST3 is **optional** and gated on whether you actually want NodeSynth in a DAW. It is a separate target (`nodesynth_vst3` in CMake), not a refactor of the existing standalone app. If the answer to "should this run as a plugin?" is "not yet", skip 5d entirely and ship Phase 5 as 5a + 5b + 5c.

**If 5d is in scope, decisions to lock:**

- **SDK choice:** Steinberg's official VST3 SDK (`vstsdk3`) via FetchContent. JUCE is rejected — way too much surface area for a single plugin format.
- **Audio thread:** the host's audio thread *is* the audio thread. The existing miniaudio thread shuts off in plugin mode (compile guard `NODESYNTH_PLUGIN`). The compiled `FAudioGraph` and SPSC command queue carry over unchanged — both are already host-thread-safe.
- **MIDI:** VST3 events replace `FMidiInput`. The plugin's `process` translates `Event::kNoteOnEvent` / `kNoteOffEvent` directly to `FVoiceAllocator::HandleNoteOn`. Existing `FMidiInput` is unused in plugin mode.
- **GUI:** Dear ImGui inside the host's plugin window. There's a battle-tested `imgui-vst3-bridge` repo for this; if it doesn't compile cleanly, fall back to a no-GUI generic-host VST3 (host shows the parameter list, no node editor). Generic-host fallback ships in days; full ImGui inside the plugin window is weeks.
- **Parameter exposure:** every node param becomes a VST3 parameter. Param IDs are `(NodeId << 16) | ParamIndex`. Limits: VST3 spec allows 32-bit param IDs, so this fits. No automation lanes for non-numeric params (Choice / Bool ride along as quantised floats).
- **Patches:** plugin presets *are* patches. The plugin's `getState` / `setState` calls `SavePatch` / `LoadPatch` to a memory buffer. Trivial.

This is a **week of work minimum** for a generic-host fallback, **multiple weeks** for full GUI parity. Decide before starting whether the payoff is there.

---

## 2. Sub-phases

```
5a (Presets)             ──┐
5b (Stereo)              ──┼──── ship Phase 5
5c (Oversampling + Mods) ──┘
5d (VST3) ────────── DEFERRED — user opted out 2026-05-02
```

5a is independent of 5b/5c (presets work fine in mono). 5b is the architectural change everything else benefits from; do it before 5c.

5d is **not in scope** for the foreseeable future. The plan stays in this doc as a pointer in case demand changes, but treat Phase 5 as closed once 5c lands.

### Phase 5a — Presets (~1 day)

- **Bundled presets folder.** CMake `install` rule copies `presets/` next to the binary. Initial set: 8–12 hand-tuned `.json` patches across Init / Bass / Lead / Pad / FX.
- **Preset menu.** New **File → Preset** submenu, populated by scanning the install dir's `presets/` and `~/.nodesynth/user_presets/`. Nested by subdirectory. Click loads via `LoadPatch`. Doesn't replace `Open…` for ad-hoc files.
- **Preset author workflow.** Document in `docs/AUTHORING-PRESETS.md` how to save a patch, drop it into the source tree's `presets/`, and rebuild — for the user to add presets without touching code.

**Done when:** the File menu has a Preset submenu with at least 8 categorised presets, each loads cleanly and produces audio. Loading a 96 kHz preset on a 48 kHz device shows the existing sample-rate-mismatch warning (free, already implemented in 4.5b).

### Phase 5b — Stereo (~5–7 days)

- **Buffer width change.** `TNodeBase` storage becomes `float[NumOutputs][2][BlockSize]`. `INode::GetOutputBuffer` signature gets a channel index, or — better — returns a struct/pair pointing at L and R. Decide one consistent API before touching nodes.
- **Mono-broadcast migration.** Every existing node updated to write the same value to both channels in `Process`. Mechanical; expect ~2 hours per node × 25 nodes = ~5 days, much of it parallelisable since each node is independent.
- **Sink update.** `FOutput::Process` reads channel 0 → device L, channel 1 → device R. miniaudio callback in `main.cpp` updated accordingly.
- **Stereo-aware migrations.** `FReverb` (already stereo internally — un-downmix), `FDelay` (two independent lines).
- **Test sweep.** Existing single-channel asserts become channel-0 asserts; add channel-1 asserts where they prove the broadcast invariant; add stereo-specific asserts on `FReverb` and `FDelay` that L ≠ R when expected.

**Done when:** all 181 existing tests still pass after the stereo migration; loading any pre-stereo patch produces audible sound on both speakers; `FReverb` and `FDelay` have measurable L/R difference under stereo input.

### Phase 5c — Oversampling + Chorus + Flanger (~3–4 days)

- **`FOversampler` utility.** Polyphase half-band FIR up- and down-sampler in `src/dsp/Oversampler.h`. ~31-tap default. Pre-allocates buffers in `Prepare`. Tests assert frequency-response (passband flat to ~0.45 Nyquist, stopband down >60 dB) and recursion correctness for 4x.
- **`FWaveshaper` opt-in.** Adds `Param_Oversample` (Choice: 1x / 2x / 4x). At 1x, behaves identically to today (regression-safe). At 2x/4x, wraps the existing per-shape transfer functions in upsample → process → downsample.
- **`FOscillator` opt-in.** ~~Same Param_Oversample. Marginal benefit at low pitches; meaningful at the top octave for Square / Saw.~~ **Deferred to a v2 follow-up.** PolyBLEP already mitigates oscillator aliasing; the implementation pattern is meaningfully more complex than `FWaveshaper`'s (the per-sample loop has stateful smoothers, phase accumulator, PolyBLEP corrections — all of which need re-prepared at 2x rate). Plan note about "marginal benefit" applies; ship `FWaveshaper` opt-in alone for v1 and revisit if user demand surfaces.
- **`FChorus` node.** New node, `src/dsp/Chorus.h`. Stereo, params per §1.6.
- **`FFlanger` node.** New node, `src/dsp/Flanger.h`. Stereo, params per §1.6, with feedback path.
- **Preset coverage.** Add at least one preset that uses each new effect, dropped into `presets/fx/`.

**Done when:** FFT of a square-wave through `FWaveshaper` at 4x oversampling shows aliased images down >40 dB compared to 1x; `FChorus` produces audible width on a mono oscillator input; `FFlanger` with feedback>0.5 produces the characteristic comb-filter sweep.

### Phase 5d — VST3 wrapper (~1–3 weeks, optional)

Don't start without explicit go-ahead. If green-lit:

- **CMake target.** `nodesynth_vst3` separate from `nodesynth`. Conditional compile guard `NODESYNTH_PLUGIN`. miniaudio thread compiled out; RtMidi compiled out.
- **VST3 SDK integration.** FetchContent on Steinberg `vstsdk3`. Single processor + controller pair. Patch state via `SavePatch` to memory.
- **Parameter mapping.** Each node param becomes a VST3 parameter, IDs `(NodeId << 16) | ParamIndex`. Choice/Bool quantised to floats.
- **MIDI.** VST3 events → `FVoiceAllocator::HandleNoteOn/Off` directly.
- **GUI.** First pass: generic host (host shows the param list, no node editor). Stretch: ImGui inside the plugin window via `imgui-vst3-bridge`. Decide based on bridge stability.

**Done when:** plugin loads in at least one host (REAPER for testing); host can play notes; preset state survives a host save+reload cycle.

---

## 3. Sequencing & budget

```
Day 1:      5a (Presets)
Day 2-8:    5b (Stereo)
Day 9-12:   5c (Oversampling + Chorus + Flanger)
[Day 13-30: 5d (VST3) — optional]
```

Total without VST3: **~12 focused days**.
Total with VST3 generic-host: **~3 weeks**.
Total with VST3 + ImGui plugin window: **~5+ weeks**.

If 5b lands but 5c slips, Phase 5 still ships as a stereo+presets release.

---

## 4. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Stereo migration breaks half the test suite at once | Big-bang on a feature branch, fix all tests in the same PR. The win-condition is "all existing tests still pass after migration"; nothing merges until that's true. |
| `INode::GetOutputBuffer` signature change cascades through every node | Make the decision once and apply with a sed-able rename. Prefer returning a tiny struct (`FStereoBuffer { float* L; float* R; }`) that's a drop-in for `float*` everywhere. |
| `FOversampler` half-band filter ringing colours the dry signal at 1x | At 1x the oversampler must be a true bypass — short-circuit to a passthrough, never run the filter chain. Test asserts bit-identical output at 1x vs. no-oversampler. |
| Stereo `FReverb` is louder than the current downmixed mono version | Compensate at the wet output — halve the wet gain so peak loudness stays consistent. Document the change in Phase 5 release notes. |
| Bundled-preset paths break on macOS .app bundles vs Windows install dir | `MACOSX_BUNDLE` puts presets inside `.app/Contents/Resources/presets/`; on Windows/Linux they sit next to the binary. Wrap the lookup in a `GetPresetsDir()` helper that handles both. |
| Chorus/Flanger LFO clicks at rate=0 (DC offset) | Clamp rate to ≥0.01 Hz. At 0 Hz the user means "no modulation"; let them set Depth=0 instead. |
| VST3 host-thread realtime guarantees differ from miniaudio | Identical in practice — both call `process` on a high-priority thread with no allocation/lock allowed. Existing realtime rules in CLAUDE.md apply unchanged. |
| VST3 ImGui-in-plugin-window bridge breaks with each VST3 SDK update | Pin `imgui-vst3-bridge` to a known-good commit via FetchContent's `GIT_TAG`. Generic-host fallback always available if the bridge breaks. |
| User presets in `~/.nodesynth/user_presets/` shadow bundled ones with the same name | Merge order: bundled first, user second; user wins on conflicts. Document this. Or: namespace user presets in their own submenu. Pick one in 5a. |

---

## 5. What stays deferred

These remain on the parking lot:

- **Pan param on `FGain` / `FVca`.** Useful, not blocking. Phase 6.
- **Dedicated stereo width / mid-side processing node.** Phase 6.
- **Convolution reverb.** Freeverb covers v1; convolution is a separate engineering lift. Phase 6+.
- **Plate / hall reverb algorithms.** Same as above.
- **External clock sync** (MIDI clock, Ableton Link, ReWire). Each is its own meaty integration.
- **Tap-tempo on `FDelay` / `FChorus` / `FFlanger`.**
- **Time-stretch / pitch-shift nodes.**
- **Granular / wavetable oscillators.**
- **Multi-allocator UI affordance.** Still no good reason — broadcast works fine when there's only one allocator.
- **Structural commands through the SPSC queue.** Big architectural lift; nothing in Phase 5 requires it.
- **MPE.** New command shape; bigger than this phase.
- **Preset tagging / search / favorites.** v1 presets browse by directory. Tags are Phase 6+.
- **AU / AAX wrappers** (Apple, Avid plugin formats). VST3 covers most DAWs; the rest is per-format engineering.
- **AAX / AU SDKs** are licensed and require developer agreements — defer until VST3 proves the demand exists.

If user feedback flips any of these from "deferred" to "blocking", revisit then — not now.
