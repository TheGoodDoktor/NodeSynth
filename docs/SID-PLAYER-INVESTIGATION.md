# SID Player Investigation

A side-project investigation requested 2026-05-02. Goal: assess the feasibility of adding a SID (Commodore 64 sound chip) tune player to NodeSynth such that the SID's register stream — voice frequencies, pulse widths, ADSR settings, filter cutoff, etc. — is exposed as Control inputs into the NodeSynth graph. The user could then play classic SID tunes through NodeSynth's own DSP (modern oscillators, filter, reverb), or use the SID as a slow-rate modulation source against unrelated patches.

This document is **not** a commitment to build it. It's an investigation: what's actually involved, what library options exist, where the licensing/architecture sharp edges are, and what an MVP looks like.

---

## 1. The idea, restated

Two goals, in priority order:

1. **Replay SID tunes** — load a `.sid` file from the High Voltage SID Collection (HVSC) and have it play out of NodeSynth.
2. **Tap the register writes** — every time the tune's player code writes to a SID register (frequency, gate, ADSR, filter cutoff, …), surface that value as a Control output buffer that other NodeSynth nodes can consume.

The interesting part is (2). (1) is a solved problem any tune-player project can do. (2) lets you re-imagine old tunes through new DSP — drive an SVF cutoff from the SID's filter sweep, route SID gates as polyphony triggers, layer a stereo reverb tail on the original chiptune, etc.

---

## 2. Background

### The SID chip in 60 seconds

The MOS 6581 (and later 8580) has 25 write-only registers at `$D400-$D418` controlling:

- **Three voices**, each with:
  - Frequency: 16-bit, `Hz = freg × Phi / 2²⁴` where Phi is the system clock (≈985.25 kHz PAL, 1022.73 kHz NTSC).
  - Pulse width: 12-bit (only meaningful for pulse waveform).
  - Control register: gate, sync, ring-mod bits + waveform select (triangle/saw/pulse/noise — multiple bits combinable).
  - ADSR envelope: two 8-bit registers carrying four 4-bit values (A/D/S/R), each indexing into a non-linear time table.
- **Global filter**: 11-bit cutoff, 4-bit resonance, 3-bit per-voice routing, 4-bit type select (LP/BP/HP, "voice 3 off" bit).
- **Volume**: 4-bit master.

The chip is famously quirky — the 6581's analog filter is non-linear and varies between individual chips; the volume register can be abused as a 4-bit DAC for sample playback (the "volume tricks" used in Galway tunes). Any serious player has to model these.

### SID tune file format (.sid / PSID / RSID)

A `.sid` file is **not** a stream of register writes**.** It's a 0x7C-byte header (PSID v1/v2 or RSID v3) followed by a **6502 machine-code program** that, when executed in a simulated C64, drives the SID by writing to `$D400-$D418`. To play a SID tune you need:

1. A **6502 CPU emulator** (no graphics, no I/O — just CPU + 64 KB RAM).
2. The PSID header parser to know where to load the code, the init/play addresses, the timer mode (VBI vs CIA), the song count, model preferences (6581 vs 8580), and PAL/NTSC.
3. A **timer** that fires the play routine at the tune-specified rate. PAL VBI = 50 Hz, NTSC VBI = 60 Hz, CIA timer is tune-defined (often 50/60 Hz, sometimes higher for "multispeed" tunes).
4. A **SID register file** that captures the writes — and, if you also want audio out, a **SID emulator** that turns the register stream into samples.

Per the HVSC reference, the PSID header's `speed` field is a 32-bit big-endian bitmap with bit N corresponding to subtune N+1; `0` = VBI interrupt, `1` = CIA-1 timer interrupt. For default (CIA mode), CIA-1 timer A is set to 60 Hz with IRQs active.

The crucial implication for our use case: **register writes occur at the player's tick rate, not the audio sample rate.** A 50 Hz tick is one write-burst every 20 ms — 960 audio samples at 48 kHz. Most NodeSynth blocks (64 samples ≈ 1.3 ms) will see zero register changes; a few will see a burst.

### High Voltage SID Collection

[HVSC](https://www.hvsc.c64.org/) is the canonical library — ~58,000 SID tunes, all in `.sid` format, freely downloadable. Any SID player should target HVSC compatibility as the bar.

---

## 3. Architectural options

Three plausible designs, ordered from least to most ambitious:

### A. Audio-only `FSidPlayer` node

Single node, one Audio output. Internally runs `libsidplayfp` (or equivalent). Pumps samples into NodeSynth's audio path like any other source.

- **Pros:** simplest possible. Few days of work. User hears their SID tunes playing through NodeSynth's effect chain.
- **Cons:** treats the SID as an opaque sample source. Doesn't address the *interesting* goal (using SID register state as modulation).

### B. Register-tap `FSidPlayer` node (no audio)

Single node, no audio output, **many** Control outputs. Runs a 6502 emulator + PSID timer; intercepts every write to `$D400-$D418`. Maps each register to a Control buffer that gets re-sampled at the audio block rate.

- **Pros:** delivers the interesting goal directly. Doesn't need a SID emulator (huge complexity reduction). Choice of 6502 emulator is wide-open including public-domain options — avoids the libsidplayfp GPL question.
- **Cons:** no original SID audio for reference. ~24 Control outputs is a lot of pins on one node — might want to split into per-voice tap nodes.

### C. Hybrid `FSidPlayer` (audio + register tap)

Single node, one Audio output **and** the Control outputs from option B. The audio comes from a real SID emulator running in parallel with the register tap (or downstream of it — the tap inspects writes before forwarding to the emulator).

- **Pros:** best of both. Original audio for reference / hybrid layering, plus modulation.
- **Cons:** combines the costs of A and B. Pulls in libsidplayfp (or another SID emulator) with its GPL implications.

**Recommended progression:** ship **B first**, add **A** later as an optional second output (gated on the licensing decision). Most of the value is in the register tap; audio playback can be retrofitted without breaking the tap design.

---

## 4. NodeSynth integration sketch

### Node design (option B, register-tap only)

```
FSidPlayer
├── Params
│   ├── File         (string — path to .sid)
│   ├── Subtune      (Choice 0..N-1 from PSID header)
│   ├── Model        (Choice 6581 / 8580 — default from PSID, override)
│   ├── Region       (Choice PAL / NTSC — default from PSID, override)
│   └── Transport    (Choice Stop / Play / Pause)
└── Outputs (all Control)
    ├── V1_Freq      (Hz)        ─┐
    ├── V1_PWM       (0..1)       │
    ├── V1_Gate      (0/1)        │  ── per-voice (×3)
    ├── V1_Waveform  (bitmask)    │
    ├── V1_AttackMs                │
    ├── V1_DecayMs                 │
    ├── V1_Sustain   (0..1)        │
    ├── V1_ReleaseMs               │
    ├── V2_…  V3_…                ─┘
    ├── F_Cutoff     (0..1 normalised, or Hz with a model-specific curve)
    ├── F_Resonance  (0..1)
    ├── F_Routing    (bitmask: voice→filter)
    └── Volume       (0..1)
```

That's 3×8 + 4 = **28 Control outputs.** Wide. Two ways to handle:

- **One fat node**, drag a connection from only the outputs you care about. The graph editor already does drag-from-pin; unconnected outputs cost nothing.
- **Split into a transport node + per-voice tap nodes** (`FSidPlayer` + `FSidVoice1/2/3` + `FSidFilter`). Cleaner UX but the per-voice nodes need a private channel back to the transport — currently NodeSynth has no notion of that.

Recommend the fat node for v1; revisit if it feels unwieldy.

### Sample-rate plumbing

The 6502 runs at chip clock (~985.25 kHz PAL); SID samples come out at any rate the emulator chooses (typically 44.1 or 48 kHz when using libsidplayfp). NodeSynth's audio thread runs at whatever the device picked (48 kHz default), with `BlockSize = 64`. Per Process block:

1. Compute the time window: `BlockMs = BlockSize * 1000 / SampleRate ≈ 1.33 ms` at 48 kHz.
2. Step the 6502 forward by that many cycles' worth of CPU clocks (`BlockMs * Phi / 1000`).
3. The CPU triggers the play routine at the PSID-defined tick rate (50 Hz VBI fires once every ~333 audio samples at 48 kHz).
4. Each register write the player makes hits a hook that captures `(reg, val, sampleOffset_within_block)`.
5. After stepping, walk the captured writes; for each Control output, decide a per-sample value: either step (latch on write) or smooth (one-pole toward the new value over a few ms — important for `Cutoff` and `Volume` where step-changes click).

The current NodeSynth Control buffer is one float per sample. For step-style outputs (gate, waveform), latch and fill. For smoothable outputs (cutoff, volume, frequency at portamento speeds), apply `FOnePoleSmoother` per output — same pattern as Gain / Oscillator amplitude.

### Threading

- **UI thread**: `SetParamValue(File, …)` parses the .sid header, loads the bytecode into the emulator's RAM, resets the CPU, primes the PSID init routine. This is the only place that allocates.
- **Audio thread**: `Process()` advances the CPU and writes Control outputs. **No allocation.** All buffers pre-allocated in `Prepare`.
- The 6502 emulator itself is pure code over a fixed 64 KB RAM array — RT-safe.

### Polyphony interaction

The `FVoiceAllocator` already takes NoteOn/NoteOff via the SPSC command queue or a direct method call. To drive NodeSynth's polyphony from the SID's three voices, an `FSidPlayer` could optionally enqueue NoteOn when V*n*_Gate goes 0→1 and NoteOff on 1→0. The note value would come from `V*n*_Freq` mapped to MIDI. Out of scope for v1; trivial to add later.

---

## 5. Library landscape

### Audio playback (option A or C only)

| Library | License | State | Verdict |
|---|---|---|---|
| **libsidplayfp** | GPL-2.0 | v2.11.0 (Jan 2026), actively maintained | The de-facto standard. Ships with reSIDfp (floating-point reSID). HVSC compatibility is its job. **GPL infects the host binary.** |
| reSID (Dag Lem original) | GPL-2.0 | Stable, frozen | Audio engine only. Same licensing issue. |
| reSIDfp standalone | LGPL-2.1 | Inside libsidplayfp | The audio engine alone is LGPL — usable under permissive licenses if dynamically linked. But you'd still need a separate 6502 host. |
| Roll your own SID | n/a | n/a | A serious DSP project on its own. Months of work to match reSID's quality. Not realistic. |

The **GPL question** is the elephant. NodeSynth currently has no `LICENSE` file at the repo root, so the project's licensing posture is undecided. If the goal is to keep NodeSynth permissive (MIT/BSD/Apache-style), pulling libsidplayfp into the same binary is incompatible — distributing the combined work would require GPL release. Two outs:

1. **Ship the SID audio path as an optional plugin** loaded at runtime via dynamic library. The plugin itself is GPL (re-distributable separately); NodeSynth core stays permissive. Same trick projects like Audacity used historically.
2. **Just go GPL.** If NodeSynth's intended distribution model doesn't conflict, this is simpler.

### CPU emulator (all options)

| Library | License | State | Verdict |
|---|---|---|---|
| **floooh/chips** (Andre Weissflog) | **Zlib** | Header-only C, actively maintained (1400+ commits) | **Recommended.** Includes m6502, m6526 (CIA), m6569 (VIC-II), and m6581 (SID) under one permissive umbrella. See §5a. |
| fake6502 by Mike Chambers | Public domain (modern fork BSD-2-Clause) | <500 lines C, full instruction set incl. undocumented opcodes | Simpler API (read/write callbacks) but you'd have to write CIA/VIC timer logic yourself. Fine fallback. |
| redcode/6502 | BSD-3-Clause | Highly portable ANSI C | Alternative if fake6502 doesn't fit. |
| MyLittle6502 | Public domain | fake6502 + 65C02 with bug fixes | Better if fake6502 turns out to have stale bugs. |

### PSID header parsing

No library needed — the format is simple enough to parse in ~50 lines using nlohmann/json-style fixed offset reads. The HVSC `SID_file_format.txt` is the authoritative spec.

---

## 5a. floooh/chips changes the picture

After the initial draft, [floooh/chips](https://github.com/floooh/chips) surfaced as a candidate. It's worth a section of its own because it materially shifts the recommendation.

**What's in it:**
- `m6502.h` — 6502 CPU emulator
- `m6526.h` — CIA (Complex Interface Adapter, the timer chip used by CIA-mode PSID tunes)
- `m6569.h` — VIC-II (the graphics chip whose vertical-blank IRQ is the other PSID timer source)
- `m6581.h` — SID emulator
- Plus a long list of other 8-bit chips (Z80, AY-3-8910, video controllers, etc.) for unrelated retro projects

**License: Zlib.** Permissive, no copyleft, GPL-clean. Header-only standalone C99 — drops into FetchContent or just gets vendored.

**API style:** tick-based with a 64-bit "pins" word per tick. Each tick returns the updated bus state — your host code dispatches reads/writes by inspecting the pin bits. More verbose than fake6502's callback model (~2× the integration code), but more accurate to real hardware behaviour and consistent across all the chips. Consistency matters here because the PSID environment needs **CPU + CIA + (optionally VIC + SID) ticked together** — having all four use the same pin convention saves glue code in the host loop.

**SID emulator quality:** simplified. The header credits its lineage to *tedplay* (2012, a TED chip emulator that grew SID support) rather than reSID, and the implementation has clear "FIXME" markers around the analog-filter quirks and oscillator-3 randomness that reSID models in detail. For *register-tap purposes this is irrelevant* — the tap inspects writes before the SID runs them; the chip's downstream interpretation doesn't matter. For *audio playback*, m6581 will sound recognisably SID-like and play HVSC tunes correctly tempo-wise, but it won't match reSIDfp on the trickier tunes (Galway-style sample tricks, 6581-filter-reliant Hubbard pads). Treat its audio out as "good enough chiptune", not "authentic 6581".

**What this means for the architecture:**

- **The licensing risk effectively disappears.** All four chips ship under Zlib. Hybrid (option C, audio + register tap) becomes shippable from day one without a project-level licensing decision.
- **The MVP can include audio output.** Previously option A was gated on libsidplayfp/GPL. With m6581.h, you get audio for free — just plumb the SID's left/right output samples into the node's Audio Out port.
- **CIA + VIC come as drop-ins** instead of being bespoke timer code. Both PSID timer modes (VBI via VIC-II raster IRQ, CIA-mode via CIA-1 timer A) work without writing the period accounting yourself. The PSID init protocol still has to set things up correctly in RAM, but the chips themselves are off-the-shelf.
- **A future high-fidelity audio path** (libsidplayfp's reSIDfp under GPL, as an optional plugin) is still a sensible phase 2 if anyone cares about audiophile-grade SID playback. The register tap and the chiptune-grade audio don't go away when that's added — m6581 stays as the default; libsidplayfp becomes an opt-in alternative SID backend.

**Slight cost vs the fake6502 path:** the pin-API integration loop is meatier. Roughly:

```c
uint64_t Pins = m6502_init(&Cpu, ...);
for (each tick in the audio block) {
    Pins = m6502_tick(&Cpu, Pins);            // CPU does its bit
    Pins = m6526_tick(&Cia, Pins);            // CIA timer ticks, can raise IRQ
    Pins = m6569_tick(&Vic, Pins);            // VIC raster ticks (for VBI)
    Pins = m6581_tick(&Sid, Pins);            // SID samples one tick
    if (Pins & M6502_RW)   { /* read */ } else { /* write — tap here for SID writes */ }
}
```

vs fake6502's `read6502/write6502` callbacks where the host writes:

```c
uint8_t read6502(uint16_t addr)  { /* dispatch */ }
void write6502(uint16_t addr, uint8_t val) { /* dispatch + tap */ }
```

The chips/ approach is more code per tick but the win is everything else is also already a chip — no separate "I need to track a CIA timer counter and fire an IRQ when it hits zero" loop in the host. Net: the integration cost is similar, the architecture is cleaner, the dependency story is dramatically better.

---

## 6. Recommended approach

### MVP: option C (hybrid) using floooh/chips

With floooh/chips on the table, the staged approach (tap first, audio later) collapses — both ship in the same MVP under Zlib.

```
Day 1-2:  Vendor / FetchContent floooh/chips. Stand up the four chip
          contexts (m6502, m6526, m6569, m6581) in a single tick loop.
          Implement the bus dispatch (RAM at $0000-$BFFF and $E000-
          $FFFF, SID at $D400, CIA at $DC00, VIC at $D000).
Day 3:    PSID header parser. Load .sid into RAM at the header's
          loadAddr; record initAddr / playAddr / songs / startSong
          / speed / region / model.
Day 4:    PSID init sequence. Run the init routine; arm the chosen
          timer (VIC raster line for VBI, or CIA-1 timer A for CIA
          mode); wire the IRQ handler to call playAddr.
Day 5-7:  FSidPlayer node. Audio Out port wired to m6581's sample
          stream. ~28 Control outputs from a write-tap on $D400-$D418.
          Control fill logic: step for gates / waveform select,
          one-pole smoothed for cutoff / volume / frequency.
Day 8-9:  UI: file picker, subtune selector, model/region overrides,
          transport. Property-panel custom widget showing the current
          register values + per-voice gate LEDs (cheap nice-to-have).
Day 10:   Tests. Synthetic .sid fixtures (a 6502 program that writes
          a known sequence to specific registers + verifies the tap
          captures them); a smoke test that loads a real .sid and
          asserts non-silent audio out.
```

**Total: ~2 weeks** for a hybrid MVP — register tap *and* chiptune-grade audio playback, all under Zlib, no licensing decision required.

### Stretch: high-fidelity audio path (optional)

If users care about reSID-grade audio (the difference is mostly audible on Hubbard / Galway tunes that lean on 6581 filter quirks), add `libsidplayfp` as an *alternative* SID backend behind a Choice param (`Engine: Built-in (m6581) / High-fidelity (libsidplayfp)`). The libsidplayfp path is GPL-2.0, so this either:

- ships as a separate dynamically-loaded plugin (NodeSynth core stays permissive), or
- triggers a project-level decision to ship NodeSynth as GPL.

Effort ~1 week on top of the MVP, plus the licensing-decision overhead. Gate on actual user demand — most listeners won't tell the difference.

### What **not** to do for the MVP

- Don't try to roll your own SID emulator. Reverb was easy to clone (Freeverb is 200 lines); reSID is 20× that and the analog filter modelling alone is a dissertation topic.
- Don't auto-route SID voices into `FVoiceAllocator` notes. Tempting, but conflates two separate concerns. Add it later as a config flag once the basic tap works.
- Don't try to make the register tap sample-accurate within the audio block. Quantising writes to block-start (or to the player tick rate) is fine for v1 — same approximation `FMidiInput` already makes.
- Don't render the 28 Control outputs as 28 visible pins by default. Hide unconnected ones (or add `FParamInfo::bHidden`-style "OutputInfo::bDefaultHidden" with a "Show all" toggle in the property panel). Otherwise the node eats half the screen.

---

## 7. Risks & open questions

| Risk | Mitigation |
|---|---|
| **Licensing.** libsidplayfp is GPL-2.0; NodeSynth has no declared license. | MVP using floooh/chips is Zlib end-to-end, no licensing decision required. The libsidplayfp path is only needed if reSID-grade audio fidelity becomes a goal — at which point treat it as an opt-in plugin. |
| **m6581 SID emulation is simplified, not reSID-grade.** Comments in the header acknowledge "FIXME" markers around oscillator-3 random output and the 6581 analog filter. Tunes that lean on filter quirks (mid-period Hubbard pads, Galway sample tricks) won't sound *exactly* right. | Acceptable for v1 — the register tap is the headline feature, and the audio is "good enough chiptune" reference. High-fidelity audio is the optional stretch goal in §6. |
| **Many tunes use 6581-specific filter quirks.** A register tap that surfaces `Filter_Cutoff` as a Hz value will give a *different* curve than the original chip produced — because the 6581's analog filter is non-linear and chip-to-chip variable. | Either expose normalised 0..1 (let the user re-map), or expose Hz using libsidplayfp's empirical filter curves (would require pulling in part of libsidplayfp's source). 0..1 is honest for v1. |
| **"Volume tricks" (4-bit DAC sample playback)** in Galway-style tunes write to the volume register at audio rate (~4-8 kHz). With a 50 Hz player timer, these are missed entirely — the writes happen between timer ticks, in the player's own sample loop. | Document the limitation. To capture audio-rate volume writes, the 6502 has to run continuously between ticks (which fake6502 supports) and the tap has to log writes at every CPU step, not just at tick boundaries. Adds complexity but doable. |
| **Tick-rate Control updates create zipper noise** when a slider-rate output (Cutoff, Volume) is consumed by a smoother-less consumer. | Always run a `FOnePoleSmoother` on smoothable outputs in the tap. `Gate` and `Waveform` stay step-functions. |
| **PSID v1/v2/v3 differences.** RSID (v3) tunes assume real C64 hardware features (legal sample playback, KERNAL/BASIC ROM access). Some won't run on a bare 6502 + RAM. | MVP targets PSID v1/v2 only. RSID compatibility is a separate effort. Most HVSC content is PSID. |
| **HVSC tunes assume specific C64 model timings.** Wrong PAL/NTSC → wrong tempo. | Default from PSID header; let user override. |
| **CIA timer interrupts at non-50/60 Hz** (multispeed tunes). | Standard PSID feature; the timer driver must read CIA-1 timer A from the player's writes, not assume 50 Hz. Adds a few hours but is solved territory. |
| **Stereo SID tunes** (some PSID v2 tunes use a second SID at $D420 or $D500). | v1 ignores the second chip. Document as known limitation. |
| **Test fixtures for HVSC tunes** can't ship in the repo (HVSC is freely downloadable but redistribution is a grey area). | Generate tiny synthetic .sid files in the test suite — a 6502 program that writes a known sequence to specific registers, asserts the tap captures those writes. |

---

## 8. Decision checklist

Before starting implementation, decide:

1. **Will NodeSynth declare a license?** (Currently undeclared.) If permissive, audio playback via libsidplayfp goes into a separate plugin or doesn't ship.
2. **One fat node or split per voice?** Recommend fat for v1.
3. **Step-output convention for Control buffers** — latched-step or one-pole-smoothed-to-step? Recommend per-output choice (Gate=step, Cutoff=smoothed).
4. **Subtune selection in the param system** — Choice from PSID's `songs` field, default from PSID's `startSong`. Easy.
5. **Where the 6502 / SID emulator code lives** — `src/sid/` namespace? Keeps it isolated from `src/dsp/`.

---

## 9. Bottom line

**Yes, this is feasible** and the value-per-effort is genuinely good — the register-tap design is novel (no commercial SID player I'm aware of exposes the register stream as a modulation source). With floooh/chips on the table, the MVP is ~2 weeks of focused work for a **hybrid** node (register tap + chiptune-grade audio), all under Zlib, no project-level licensing decision required. It fits cleanly into NodeSynth's existing patterns: Control outputs with optional smoothing, file-picker via the existing nfd-extended path, custom property-panel widget for the register-state visualisation.

The libsidplayfp / reSIDfp high-fidelity audio path is an optional later chapter, gated on whether anyone actually hears the difference and asks for it.

---

## Sources

- [floooh/chips](https://github.com/floooh/chips) — Zlib, header-only C; m6502 + m6526 (CIA) + m6569 (VIC-II) + m6581 (SID) under one permissive umbrella. **Recommended for the MVP.**
- [m6581.h source](https://github.com/floooh/chips/blob/master/chips/m6581.h) — the SID emulator inside floooh/chips; tedplay-derived, simplified vs reSID
- [libsidplayfp on GitHub](https://github.com/libsidplayfp/libsidplayfp) — current version 2.11.0, GPL-2.0, last updated Jan 2026; the high-fidelity alternative
- [libsidplayfp Free Software Directory entry](https://directory.fsf.org/wiki/Libsidplayfp) — license confirmation
- [SID Format Specification (OverClocked ReMix)](https://ocremix.org/info/SID_Format_Specification) — PSID/RSID file format reference
- [SID file format (HVSC, authoritative)](https://www.hvsc.c64.org/download/C64Music/DOCUMENTS/SID_file_format.txt) — header layout, speed flags, init/play addresses
- [fake6502 (Mike Chambers, public domain)](http://rubbermallet.org/fake6502.c) — drop-in 6502 emulator
- [MyLittle6502 (BSD-2-Clause fake6502 fork with bug fixes)](https://github.com/C-Chads/MyLittle6502) — recommended modern variant
- [redcode/6502 (BSD-3-Clause)](https://github.com/redcode/6502) — alternative ANSI C 6502 emulator
- [High Voltage SID Collection](https://www.hvsc.c64.org/) — the canonical SID tune library, ~58,000 tunes
