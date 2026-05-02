# SID Player Node

A NodeSynth node that loads Commodore 64 `.sid` (PSID v1/v2) tunes via the floooh/chips 6502 + SID emulators, plays them back through the audio graph, **and** exposes the SID's live register state as 28 Control outputs you can route into the rest of your patch.

The audio is "chiptune-grade" rather than reSID-grade — m6581's filter modelling is simplified — but it sounds clearly recognisable and plays HVSC content at the correct tempo. The interesting half is the register tap: re-imagine the original tune's filter sweep through a modern SVF, drive a polyphonic synth from the SID's gates, layer NodeSynth voices on top of the original audio for a hybrid effect.

## Quick start

1. Right-click the graph background → **Create Node** → **SID Player**.
2. Select the new node. In the property panel, click the **`...`** button next to the `File` field and pick a `.sid` file.
3. The status block underneath shows green **"Loaded"** with the tune's name / author / subtune count if the file parsed successfully. Red error text if not.
4. Wire the `Out` (Audio) port to your `Output` node. You should hear the tune play.

If the audio output is connected but you hear silence, glance at the three **Gates** indicator lights in the property panel:

- **All three lit, but silent** → audio output isn't reaching `Output`. Check your wiring; you'll usually need a `Gain` between SID Player and Output (the SID's full-volume output can be loud).
- **All three dark** → tune isn't actually playing. Common causes: tune relies on RSID v3 features (rejected at load), uses a multi-SID layout (also rejected), or its init didn't fully complete.

## Where to get tunes

The [High Voltage SID Collection](https://www.hvsc.c64.org/) has ~58,000 PSID files, freely downloadable. Drop them anywhere on disk; the file picker walks the OS dialog. A natural spot is `~/.nodesynth/sid/` (or `%USERPROFILE%\.nodesynth\sid\` on Windows) — sibling to the existing `imgui.ini` / `node_editor.ini` / `user_presets/` per-user state.

Saved patches store the absolute path to the .sid file. Moving the .sid breaks the patch; this is a v1 limitation, documented for now.

## What the outputs mean

Audio (1 port):
- **`Out`** — mono SID audio. Stereo is wire-broadcast (channel 0 aliased onto channel 1) per Phase 5b conventions.

Per voice (3 voices × 8 ports each):
- **`V*n*_Freq`** — oscillator frequency in Hz, smoothed (5 ms one-pole). Computed from the SID frequency-register pair: `Hz = freg × Φ / 2²⁴` where Φ is the chip clock (~985 kHz PAL).
- **`V*n*_PWM`** — pulse-width 0..1, smoothed. 0.5 is a perfect square; 0 / 1 are silent.
- **`V*n*_Gate`** — 0/1 step. Bit 0 of the SID's voice control register. Drive an `FVoiceAllocator` `NoteOn` from the rising edge to repurpose SID voices as polyphony triggers.
- **`V*n*_Waveform`** — 4-bit bitmask, step. Triangle = 1, Saw = 2, Pulse = 4, Noise = 8. Multiple bits set = combined waveforms (ring-mod / sync).
- **`V*n*_Attack` / `V*n*_Decay` / `V*n*_Release`** — ms, step. Decoded from the 4-bit ADSR table in the 6581 datasheet.
- **`V*n*_Sustain`** — 0..1, step. The 4-bit sustain nibble normalised.

Globals (4 ports):
- **`F_Cutoff`** — 0..1 normalised, smoothed. **Not Hz** — the 6581's actual cutoff curve is non-linear and chip-to-chip variable, so we surface the raw 11-bit register value normalised. Re-map to Hz via a `Scale` node if you want a specific curve.
- **`F_Resonance`** — 0..1, smoothed.
- **`F_Routing`** — 4-bit bitmask, step. V1 = 1, V2 = 2, V3 = 4, ExtIn = 8. Set bit means "route this voice through the filter".
- **`Volume`** — master volume 0..1, smoothed.

## Example patches

### Filter modulation
Drive an SVF cutoff from the SID's filter sweep:

```
SID Player.F_Cutoff ──┐
                      ▼
SID Player.Out ──→ SVF.Audio
                      Cutoff ◀ (the wire above)
                   SVF.LP ──→ Output
```

You hear the SID through your SVF, but the filter cutoff still tracks whatever the original tune was doing. Add a `Scale` between `F_Cutoff` and the SVF to re-map the 0..1 range to a Hz range you like.

### Drive NodeSynth voices from SID gates
Use V1's gate as a polyphony trigger and V1's frequency to set the note. Wire `V1_Freq` directly into an `Oscillator`'s `Freq` Control input, and `V1_Gate` into an `ADSR`'s `Gate`. Now your modern oscillator + envelope plays in time with V1 of the SID tune. Layer with the SID audio for a fattened hybrid sound.

### Filter routing as a discrete signal
`F_Routing`'s bitmask isn't directly useful as continuous CV, but you can extract individual bits:

```
F_Routing ──→ Scale (1/8, ÷ 0.125) ──→ Multiply ──→ ...
                                       (× 8 to recover, mask, etc.)
```

For most use cases, just connect it to a `Scope` to see what the tune is doing with filter routing over time.

## Params

- **`File`** — path to a `.sid` file. Edited via text or the `...` file picker.
- **`Subtune`** — 1-based index. Most HVSC tunes only have one subtune; some have many. Out-of-range values are clamped.
- **`Region`** — `PAL` (default, ~985 kHz chip clock) / `NTSC` (~1023 kHz). The PSID header may suggest a region; this overrides.
- **`Model`** — `6581` / `8580`. **Informational only in v1** — m6581 emulates a single curve regardless of which you select. The flag persists in the patch JSON for forward-compatibility.
- **`Bypass`** — when true, all outputs (audio + control) hold zero. Useful for A/B'ing patches.

## Known limitations

These are documented v1 boundaries, not bugs:

| | |
|---|---|
| **PSID v1/v2 only** | RSID v3 tunes need a real C64 KERNAL ROM and legal sample playback. Rejected at load with "RSID v3 not supported". |
| **Single SID** | Multi-SID tunes (a second SID at `$D420` or `$D500`) are rejected with "Multi-SID tunes not supported". |
| **No sample-rate audio quirks** | Galway-style "volume tricks" that abuse `$D418` writes as a 4-bit DAC sample stream are missed (writes happen between play ticks). |
| **Filter cutoff in normalised units** | The 6581's cutoff curve is chip-to-chip variable; surfacing Hz would lie. v2 may add a `F_Cutoff_Hz` second port using a stock 6581 calibration. |
| **Audio fidelity** | m6581's filter modelling is simpler than reSIDfp. Tunes that rely on Hubbard-era filter quirks won't sound exactly right. The libsidplayfp / reSIDfp high-fidelity backend is a future opt-in. |
| **File path absolute-only** | Saved patches store the absolute path. Moving the .sid file breaks the patch. v2 will support a SID-library directory + relative paths. |

## Architecture (for hackers)

The actual emulator wrapper lives in `src/sid/SidEmulator.{h,cpp}` — PIMPL'd so the C-style `chips/` headers don't leak into other translation units. The PSID parser and init-protocol orchestration are in `src/sid/PsidLoader.{h,cpp}`.

`FSidPlayer` (`src/dsp/SidPlayer.{h,cpp}`) holds an `std::atomic<std::shared_ptr<FSidEmulator>>` — the UI thread parses the .sid file, builds a fresh emulator, atomically swaps it in. The audio thread loads once per `Process` call and ticks against whatever's there. Old emulators get destroyed on whichever thread releases the last shared_ptr ref.

Per-output policy in `Process`: smoothed (Freq, PWM, Cutoff, Resonance, Volume) goes through a 5 ms one-pole; step (Gate, Waveform, ADSR fields, Routing) latches on register write.

Boot sequence in `LoadAndInit`:
1. Copy bytecode into RAM at the resolved load address.
2. `BootCpu(InitAddr)` — wires the 6502 reset vector at `$FFFC-$FFFD` and pulses RES.
3. `RunInitRoutine` — pushes a sentinel to the stack so init's `RTS` lands at `$0002` (a `JMP self` idle loop), then steps until PC drops there. Force-clears the I flag at exit so VBI/CIA IRQs can fire even if init didn't `CLI`.
4. `InstallPlayHook` — writes a small `JSR PlayAddr / RTI` stub at `$FFE0` and points `$FFFE-$FFFF` at it.
5. `SetVbiTimer` — for VBI-mode tunes, enables a virtual cycle counter that pulses M6502_IRQ at 50/60 Hz. CIA-mode tunes leave this disabled and rely on m6526's timer-A IRQ delivery.

Per-cycle bus dispatch in `TickOneCycle` ORs IRQ contributions from VBI + CIA1 + CIA2 separately each tick, since m6526's tick unconditionally rewrites the IRQ bit from its own state — without isolating per-chip IRQ output, two CIAs would clobber each other's interrupts.
