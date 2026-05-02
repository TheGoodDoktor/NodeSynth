# NodeSynth — SID Player Development Plan

A side-project feature: a `FSidPlayer` node that loads a `.sid` tune, replays it through a 6502 + SID + CIA + VIC emulator pulled from [floooh/chips](https://github.com/floooh/chips), and exposes both the SID's mono audio stream **and** the per-voice / global register writes as outputs into the NodeSynth graph.

This plan refines [`SID-PLAYER-INVESTIGATION.md`](SID-PLAYER-INVESTIGATION.md) into actionable sub-phases. Read the investigation first for the *why*; this doc is the *how*.

**Entry state:** Phases 0–5b complete. Stereo signal path live; bundled presets shipped. Plan written 2026-05-02.

**Goal:** a single `FSidPlayer` node that loads any HVSC PSID v1/v2 tune, plays its audio out a mono Audio port, and surfaces 28 Control outputs (3 voices × 8 + 4 globals) tracking the SID register state — usable as modulation sources, gates, frequencies for downstream NodeSynth nodes.

**Non-goal for v1:** RSID v3 compatibility, libsidplayfp/reSIDfp-grade audio fidelity, stereo (multi-SID) tunes, Galway-style audio-rate sample playback, SID-voice → polyphony note routing.

**Final deliverable that defines "done":** the seeded patch, plus a user-loaded `.sid` from HVSC, produces audible chiptune-grade SID audio through the existing audio chain; at least one Control output (e.g. `V1_Freq`) drives an SVF cutoff in a small example patch shipped under `presets/Demo/`. Tests cover register-tap correctness with synthetic SID fixtures.

---

## 1. Decisions to lock first

### 1.1 Library integration

[floooh/chips](https://github.com/floooh/chips), Zlib, header-only C99. Pulled in via `FetchContent` alongside the existing dependencies (GLFW, ImGui, miniaudio, RtMidi, nlohmann/json, nfd-extended). Headers needed: `m6502.h`, `m6526.h`, `m6569.h`, `m6581.h`. The whole repo is small enough that vendoring is also viable, but FetchContent matches the project convention.

```cmake
FetchContent_Declare(chips
    GIT_REPOSITORY https://github.com/floooh/chips.git
    GIT_TAG        master           # pin to a known-good commit before merging
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(chips)
target_include_directories(nodesynth PRIVATE ${chips_SOURCE_DIR}/chips)
```

`GIT_TAG master` is for the spike branch; pin to a commit hash before the feature ships so floooh/chips master changes don't break us silently.

### 1.2 Directory layout

Mirror the MIDI precedent — emulator infrastructure in its own directory, node in `src/dsp/`:

```
src/
├── sid/
│   ├── SidEmulator.{h,cpp}   # floooh/chips wrapper: CPU + CIA + VIC + SID + 64 KB RAM, tick loop
│   ├── PsidLoader.{h,cpp}    # .sid file parser, init-routine orchestration
│   └── SidRegisters.h        # register addresses, conversion helpers (freg→Hz, ADSR index→ms)
└── dsp/
    └── SidPlayer.{h,cpp}     # FSidPlayer node — pulls in the above; exposes the NodeSynth API
```

`SidEmulator` is the only translation unit that includes the floooh headers. Everything else talks to `FSidEmulator` via a clean C++ surface so we can swap the backend later (e.g. for libsidplayfp) without touching the node.

### 1.3 Bus dispatcher + memory map

The emulator owns a `std::array<uint8_t, 65536>` for RAM and routes reads/writes from m6502 to the right destination per the C64 memory map. v1 supports the minimum needed to run PSID:

| Range | Destination |
|---|---|
| `$0000-$BFFF` | RAM (read + write) |
| `$D000-$D3FF` | VIC-II registers (m6569) |
| `$D400-$D7FF` | SID registers (m6581 via tap) |
| `$D800-$DBFF` | Color RAM (writes ignored, reads return 0 — PSID tunes don't read it but might write) |
| `$DC00-$DCFF` | CIA-1 (m6526) |
| `$DD00-$DDFF` | CIA-2 (m6526 #2 — minimal use; some tunes test it) |
| `$E000-$FFFF` | RAM (read + write — no KERNAL ROM in v1) |

PSID's "PlaySID compatibility" assumption is that all of `$0000-$FFFF` is RAM with no ROM/IO except the SID/CIA/VIC ranges. That's exactly what we provide. RSID's higher fidelity (KERNAL ROM at `$E000-$FFFF`, BASIC at `$A000-$BFFF`) is the v2 lift.

### 1.4 Threading model

- **UI thread** parses the `.sid` file, copies the bytecode into the RAM array, runs the init routine, primes the timer, and atomically swaps the prepared emulator state into the audio-thread-visible field. SetParam-style file loading; no allocation on the audio thread.
- **Audio thread** drives the tick loop in `Process()`. Per audio sample: tick CPU + CIA + CIA2 + VIC + SID once each (or N times if the chip clock divides differently than the audio rate — see §1.6), inspect the bus pins for register writes, dispatch writes into the tap, append the SID's audio sample to the Audio Out buffer.
- **Reset on subtune change.** When `Param_Subtune` changes, the UI thread rebuilds the emulator state (resets CPU + chips, re-runs init with `A = subtune`) and atomically swaps. Reuses the existing snapshot-swap pattern rather than draining commands mid-block.

The "atomically swap" detail: `FSidPlayer` holds an `std::atomic<std::shared_ptr<FSidEmulatorState>>` carrying a snapshot of the emulator's internal state (RAM contents, CPU registers, chip contexts). UI thread builds a new snapshot in a fresh shared_ptr, atomic-stores. Audio thread `load()`s once at the start of each block, ticks against it for the block, the next block sees the new one. Same idiom as `FAudioState::Graph` from Phase 1.

Note: this is heavier than `SetParam`'s relaxed-atomic write — we're swapping kilobytes of state. But it only happens on file load / subtune change (not slider drags) so audio-thread cost is just the `load()`.

### 1.5 PSID parser scope

PSID **v1 and v2 only** for v1. RSID v3 is rejected with a clear error in the property panel. Header layout (big-endian, fixed offsets per the [HVSC SID file format spec](https://www.hvsc.c64.org/download/C64Music/DOCUMENTS/SID_file_format.txt)):

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | magic — `"PSID"` (accept) or `"RSID"` (reject) |
| 0x04 | 2 | version (1 or 2 accepted; 3 rejected) |
| 0x06 | 2 | dataOffset — typically 0x76 (v1) or 0x7C (v2) |
| 0x08 | 2 | loadAddr — 0x0000 means "first two bytes of the data segment are the load address" |
| 0x0A | 2 | initAddr |
| 0x0C | 2 | playAddr |
| 0x0E | 2 | songs |
| 0x10 | 2 | startSong |
| 0x12 | 4 | speed |
| 0x16 | 32 | name (null-padded ASCII) |
| 0x36 | 32 | author |
| 0x56 | 32 | released |
| 0x76 | 2 | flags (v2+) — bits encode region (PAL/NTSC/both/unknown) and SID model (6581/8580/both) |
| 0x78+ | varies | reserved, ignored in v1 |

Parser returns a `FPsidHeader` struct on success or an error code (file-too-short, bad-magic, version-too-new, RSID-not-supported). Error surfaces to the property panel as a red one-line message.

### 1.6 Sample-rate conversion

The C64 master clock is 985,248 Hz (PAL) or 1,022,727 Hz (NTSC). m6581 internally divides this to produce one sample per chip clock — i.e. it's the chip-clock-rate audio stream. NodeSynth audio runs at the device's rate (48 kHz default).

Conversion approach: **fixed-step decimation with a tiny lowpass.** Per audio sample:
1. Tick all chips `ChipTicksPerSample = ChipClock / SampleRate` times — about 21 ticks per audio sample at 985.25 kHz / 48 kHz.
2. The fractional part accumulates a phase counter; when it crosses 1, take an extra tick.
3. The SID's per-tick output samples are summed inside the inner loop, divided by the tick count to yield the audio sample written to the Audio Out buffer.

This is "boxcar averaging decimation" — not the prettiest filter, but it removes obvious aliasing and the SID's own analog roll-off ate everything above ~6 kHz at the source anyway. Cleaner polyphase decimation is a follow-up.

If `m6581_tick` returns "sample ready" via a pin bit (likely; that's the standard floooh/chips pattern), we read the sample from the chip's internal sample register on the cycles it signals; otherwise we just read the post-DAC analog state every tick. The exact pin protocol gets nailed down in Sid.1 once the API is in hand.

### 1.7 Register tap design

The tap watches every CPU write to `$D400-$D418` (SID register file). Each write captures `(reg_index, value, sample_offset_within_block)`. After all chip ticks for the block are done, the tap walks the captured writes and fills the 28 Control output buffers.

**Per-output policy** — picked once at design time, not configurable:

| Output | Type | Update style | Smoother TC |
|---|---|---|---|
| V*n*_Freq | Float (Hz) | One-pole smoothed | 5 ms |
| V*n*_PWM | Float (0..1) | One-pole smoothed | 5 ms |
| V*n*_Gate | Bool (0/1) | Step (latched) | — |
| V*n*_Waveform | Float (bitmask 0..15) | Step | — |
| V*n*_Attack | Float (ms) | Step | — |
| V*n*_Decay | Float (ms) | Step | — |
| V*n*_Sustain | Float (0..1) | Step | — |
| V*n*_Release | Float (ms) | Step | — |
| F_Cutoff | Float (0..1 normalised) | One-pole smoothed | 5 ms |
| F_Resonance | Float (0..1) | One-pole smoothed | 5 ms |
| F_Routing | Float (bitmask 0..15) | Step | — |
| Volume | Float (0..1) | One-pole smoothed | 5 ms |

ADSR fields are rarely modulated mid-tune (they're set at note-on and stay), so step is safe and avoids smoothing surprise on note retrigger. Frequency, PWM, and filter are commonly swept — they smooth.

**Bitmask outputs.** `V*n*_Waveform` and `F_Routing` carry packed bit fields (Triangle=1, Saw=2, Pulse=4, Noise=8 for waveform; V1=1, V2=2, V3=4, ExtIn=8 for filter routing). Downstream consumers either decode bits manually with `Multiply` + `Scale` nodes, or treat them as opaque values that drive a Choice-style discrete behaviour. Documented; no automatic decoding in v1.

### 1.8 Param mappings

Conversion helpers in `src/sid/SidRegisters.h`:

```cpp
inline float SidFreqToHz(uint16_t FregLo, uint16_t FregHi, double ChipClock)
{
    const uint32_t Freg = (FregHi << 8) | FregLo;
    return static_cast<float>((Freg * ChipClock) / 16777216.0);  // 2^24
}

inline float SidAdsrAttackMs(uint8_t Index);    // lookup into [2, 8, 16, 24, 38, 56, 68, 80, 100, 240, 480, 800, 1000, 3000, 5000, 8000]
inline float SidAdsrDecayReleaseMs(uint8_t Index);  // ×3 of the attack table per the SID datasheet
inline float SidPulseWidthRatio(uint16_t Pwlo, uint16_t Pwhi);   // 12-bit (0..4095) → (0..1)
```

ADSR tables come straight from the MOS 6581 datasheet (the same tables are in every SID emulator). Hard-coded as constexpr arrays.

**Filter cutoff stays normalised 0..1** for v1. The 6581 cutoff curve is non-linear and chip-to-chip variable; surfacing Hz would lie. A future `F_Cutoff_Hz` second port using a stock 6581 curve is deferred.

### 1.9 File path persistence in patches

Saved patches store `Param_File` as **absolute path**. When loading on a different machine, the file resolves only if the path exists; otherwise the SID node loads with an error in the property panel and silence on the audio output. Documented limitation.

A v2 enhancement: store the path relative to a user-configured "SID library directory" (e.g. `~/.nodesynth/hvsc/`). For v1, absolute is honest about its limits.

### 1.10 Audio output channels

`FSidPlayer` writes mono — only channel 0 of `OutputBuffer[0]`. `IsOutputStereo(0)` returns false (default). The Phase 5b wire-broadcast convention takes care of the rest: downstream consumers see identical L/R from this source, which is correct for mono SID.

### 1.11 Error handling

Property panel renders one of:

- *(empty file path)*: "No file loaded. Load a .sid file via the file picker."
- *Loaded successfully*: shows `name`, `author`, `released` from PSID header; `subtune N of M`; chip clock (PAL/NTSC); model (6581/8580); a green ✓.
- *Load failed*: red text with the specific reason — "File not found", "Not a SID file (bad magic)", "RSID v3 not supported", "Multi-SID tunes not supported (v1 limitation)", "File truncated".

The audio output on a failed load is silence; Control outputs hold last-known values from the previous successful load (or 0 if no tune ever loaded).

### 1.12 Testing strategy

Real HVSC tunes can't be redistributed in the test fixture (HVSC's redistribution policy is grey). Tests use **synthetic .sid files** — small hand-constructed PSID v1 binaries containing a tiny 6502 program that writes a known register sequence and `RTI`s. Catch2 fixtures in `tests/fixtures/sid/` produce these from a hex-string definition at test start (so the .sid bytes are inline in the test, not committed).

Three classes of tests:

1. **PSID parser** — feed malformed headers (truncated, RSID, unsupported version, bad magic), assert correct error codes and no crash.
2. **Bus dispatch** — boot an emulator, run a synthetic program that writes 0x42 to $D404, tick once, assert the SID register at index 4 received 0x42 and the tap captured the write.
3. **Control output filling** — boot a synthetic tune that sets V1_Freq to a known register pair, run for 100 audio samples, assert `V1_Freq` Control buffer holds the expected Hz value (within smoother tolerance).

A separate optional smoke test (skipped in CI, gated on `[hvsc]` tag like `[preset-emit]`) loads a real `.sid` file from a user-specified path (env var `NODESYNTH_HVSC_PATH`) and asserts the audio output is non-silent over 1 second. Runs locally only.

---

## 2. Sub-phases

```
Sid.1 (chip scaffold)         ──┐
Sid.2 (PSID parser + boot)    ──┼──── ship as one feature
Sid.3 (FSidPlayer node)       ──┘
Sid.4 (UI polish + docs)       ── nice-to-have
```

Sid.1–3 are the required path. Sid.4 is polish — could land separately if the rest takes longer than budget.

### Sid.1 — Chip scaffold (~2 days)

**`src/sid/SidEmulator.{h,cpp}`** wraps floooh/chips behind a clean C++ surface.

API (sketch):

```cpp
class FSidEmulator
{
public:
    void Reset();                                     // Reset CPU + all chips, zero RAM
    void LoadIntoRam(const uint8_t* Data, size_t Size, uint16_t LoadAddr);
    void RunInitRoutine(uint16_t InitAddr, uint8_t Subtune);
    void ArmPsidTimer(EPsidTimer Mode, uint16_t TickRateHz);
    void SetIrqHandler(uint16_t PlayAddr);

    // Audio-thread API.
    struct FRegisterWrite { uint8_t Reg; uint8_t Value; uint16_t SampleOffset; };
    void TickForAudioSamples(uint32_t NumAudioSamples,
        std::vector<FRegisterWrite>& OutWrites,    // pre-allocated buffer in caller
        float* OutAudio);                          // length NumAudioSamples
private:
    std::array<uint8_t, 65536> Ram;
    m6502_t Cpu;
    m6526_t Cia1, Cia2;
    m6569_t Vic;
    m6581_t Sid;
    uint64_t Pins;
    double ChipClock = 985248.0;       // PAL default; NTSC = 1022727
    uint32_t TicksPerAudioSampleQ16;   // fixed-point ratio
    uint32_t TickPhaseQ16;             // accumulator for fractional ticks
};
```

**Done when:** a hand-written 12-byte 6502 program (`LDA #$42; STA $D404; RTI`) loaded at $1000 and stepped one tick produces (a) the value 0x42 in m6581's register state, (b) one entry in the OutWrites buffer with `Reg=4, Value=0x42`. Test asserts both.

### Sid.2 — PSID parser + boot sequence (~2 days)

**`src/sid/PsidLoader.{h,cpp}`** parses .sid files and orchestrates the init protocol.

```cpp
struct FPsidHeader {
    uint16_t Version;
    uint16_t LoadAddr;       // 0 means "embedded in first 2 bytes of data segment"
    uint16_t InitAddr;
    uint16_t PlayAddr;
    uint16_t Songs;
    uint16_t StartSong;      // 1-based
    uint32_t Speed;          // bitmap: bit N = subtune N+1, 0=VBI 1=CIA
    std::string Name;
    std::string Author;
    std::string Released;
    uint16_t Flags;          // v2+ — region + model bits
};

enum class ELoadError { None, FileTooShort, BadMagic, UnsupportedVersion, RsidUnsupported, MultiSidUnsupported };

struct FLoadedSidTune {
    FPsidHeader Header;
    std::vector<uint8_t> Bytecode;   // ready to copy into RAM at LoadAddr
    EPsidTimer DefaultTimer;         // VBI or CIA per Speed bit for StartSong
};

std::variant<FLoadedSidTune, ELoadError> LoadSidFile(const std::filesystem::path& Path);
```

The init protocol:
1. Reset emulator.
2. Copy `Bytecode` into RAM at `LoadAddr` (or, if `LoadAddr == 0`, at the address from the first two bytes of the data segment — Commodore .prg-style header).
3. Set CPU `A = subtune - 1` (PSID convention; some tunes use A directly, others ignore it and read a global). Both work as long as we set A.
4. Set CPU `PC = InitAddr`. Set the stack such that an `RTS` from init returns to a known sentinel ($EA31 traditionally — fall through to a `BRK` we install).
5. Run the CPU until it hits the sentinel or a configurable instruction-count timeout.
6. Once init returns, install an IRQ vector at $FFFE pointing to a small stub that JSRs `PlayAddr` and RTIs. The stub itself lives at a fixed spot in RAM (e.g. $0314 — the standard C64 IRQ vector indirection).
7. Configure the timer per `Speed` bit for the chosen subtune: VBI mode arms VIC-II raster IRQ on line 0xFA; CIA mode arms CIA-1 timer A with the period the player code writes (or default $4025 = ~50 Hz if unspecified).

**Done when:** load a synthetic PSID v2 fixture whose init writes 0xFF to V1_Volume and whose play writes V1_Freq=440 Hz on every tick. After init: $D418 register = 0x0F (the volume nibble). After 1 tick of the timer fires: V1_Freq registers reflect 440 Hz.

### Sid.3 — FSidPlayer node (~5 days)

**`src/dsp/SidPlayer.{h,cpp}`** is the actual NodeSynth node.

Layout:
- 0 inputs.
- 29 outputs total: 1 Audio Out (mono) + 28 Control Out.
- Params: `File` (string), `Subtune` (Choice 1..N), `Region` (Choice PAL/NTSC, default from header), `Model` (Choice 6581/8580, default from header), `Transport` (Choice Stop/Play/Pause).

`Param_File` is special — it's a path string, not a float. NodeSynth's existing param system is float-only. Two options:

1. **Add a string-param kind** (`EParamKind::String`) to the param infrastructure. New widget in property panel (text field + file-picker button). Serialise to JSON as a string. Touches the param system project-wide.
2. **Path lives outside the param system.** UI accesses `FSidPlayer::SetFilePath(std::filesystem::path)` directly; save/load roundtrips via a node-specific extension to the patch JSON (next to params, under a `node_extra` key). Less invasive, more bespoke.

Recommend **option 1** — adding `EParamKind::String` is a one-day lift and unlocks future nodes (sampler with audio file path, preset-load nodes, etc.). Sequencer's per-step grid already established the precedent of richer-than-float param data via `bHidden` + custom UI; string-typed params is the next natural step.

Process loop sketch:

```cpp
void FSidPlayer::Process(const FProcessContext& Ctx)
{
    auto State = CurrentEmulator.load();          // shared_ptr atomic load
    if (!State || !bPlaying.load(std::memory_order_relaxed))
    {
        // Output silence + freeze Control buffers at last values.
        return;
    }

    State->TickForAudioSamples(Ctx.BlockSize, ScratchWrites, GetOutputBuffer(Audio_Out, 0));

    // Walk captured writes, fill Control outputs with step or smoothed values.
    DispatchWritesToControlOutputs(ScratchWrites, Ctx.BlockSize);
}
```

`ScratchWrites` is pre-allocated at construction (typical capacity ~32 writes per block under PSID's 50 Hz tick rate).

**Done when:** a synthetic .sid that drives V1_Freq through a known sequence — say 220 → 440 → 880 over three ticks — produces an audio output that's clearly periodic (FFT shows the expected fundamentals), and the V1_Freq Control output reads ≈220, ≈440, ≈880 at the right sample offsets (within smoother tolerance).

### Sid.4 — UI polish + documentation (~1 day)

- **Property-panel custom UI** — a small block showing the loaded tune's `name`, `author`, `released`, `subtune N of M`, region/model, and three gate-state indicator lights (one per voice) updated live. The lights tap the same `V*n*_Gate` writes the Control outputs see; no extra data path.
- **File picker** — reuses the existing nfd-extended path. Filter: `*.sid`. The popup-style typed-path fallback also handled.
- **`docs/SID-PLAYER.md`** — usage guide. How to load HVSC tunes, what each Control output means, suggested patches (e.g. "drive an SVF cutoff from F_Cutoff", "use V1_Gate as a polyphony trigger").
- **One demo preset** in `presets/Demo/SID Filter Mod.json` — loads a placeholder SID (or just no SID, with a note) but wires `F_Cutoff` to an SVF for the user to discover by example. The preset can't ship a real .sid file, so it ships with `Param_File` empty and a property-panel hint.

---

## 3. Sequencing & budget

```
Day 1-2:   Sid.1 (chip scaffold)
Day 3-4:   Sid.2 (PSID parser + boot)
Day 5-9:   Sid.3 (FSidPlayer node)
Day 10:    Sid.4 (polish + docs)
```

Total: **~2 weeks**, matching the investigation report estimate.

If `EParamKind::String` (§Sid.3 option 1) takes longer than the 1-day budget, fall back to option 2 for v1 and ship the param kind as a separate follow-up.

---

## 4. Risks & mitigations

| Risk | Mitigation |
|---|---|
| floooh/chips API surprises us — pin convention, sample-ready protocol, clock divider details. | Sid.1 spends two days proving out the tick loop with a synthetic test before any node code. If something fundamental doesn't work, we surface it early enough to consider fake6502 + write-our-own-CIA as a fallback. |
| **m6581 audio quality is "chiptune-grade", not reSID-grade.** Some HVSC tunes (Hubbard pads, Galway sample tricks) won't sound right. | Ship as-is for v1; document in the property panel and `docs/SID-PLAYER.md`. The high-fidelity libsidplayfp path is the documented v2 enhancement. |
| **PSID init routine fails to terminate.** Some malformed tunes loop forever in init. | Cap the init run at 1,000,000 CPU instructions (~1 s of C64 wall-clock); abort with "Init routine timed out" error if exceeded. |
| **64 KB RAM allocation on the audio thread**. | Pre-allocated as `std::array<uint8_t, 65536>` member of `FSidEmulator`. Construction happens on the UI thread (FSidPlayer constructor); audio thread only reads/writes within. |
| **Subtune switch is heavy** (full state rebuild). Done on UI thread; atomic shared_ptr swap to audio thread. Could glitch if multiple swaps happen between blocks. | Acceptable v1 behaviour — subtune change is a deliberate user action, brief audio interruption is tolerable. SetParam command queue avoids inter-swap glitches for routine param updates (Region, Model, etc.) since those don't rebuild state. |
| **HVSC tune can't be bundled** in the repo for testing (redistribution grey area). | Tests use synthetic hand-written .sid fixtures; an optional smoke-test suite gated on env var loads user-supplied tunes locally. |
| **File path absolute-only** breaks on patch portability. | Documented v1 limitation; v2 introduces a SID-library-directory + relative paths. |
| **Multi-SID tunes** (PSID v2 with second SID at $D420 or $D500) — common in modern HVSC content. | Reject with "Multi-SID tunes not supported (v1 limitation)" error. The PSID v2 flags word indicates multi-SID; check at load time. |
| **CPU-intensive ticks** — running 4 chips × 21 ticks per audio sample could blow the audio budget on slow machines. | Profile in Sid.1. Budget guess: <2 % of audio thread on a typical desktop. If it's a problem, m6526/m6569 could be ticked once per block instead of per sample (CIA timer accuracy degrades but not perceptibly). |
| **`EParamKind::String` ripples through more code than expected** — patch serializer, history, property panel, palette. | Time-box at 1 day. If overruns, fall back to bespoke file-path handling on `FSidPlayer` and ship the kind in a follow-up. |
| **`Speed` field interpretation differs across PSID versions** (default for unset bits, 32-bit big-endian byte order). | Follow the HVSC spec literally; cross-check against [ChiptuneSAK's docs](https://chiptunesak.readthedocs.io/en/stable/sid.html). |
| **Galway-style audio-rate volume samples** are missed by tick-rate sampling. | Out of scope for v1. The CPU runs continuously between IRQs anyway, so the writes *are* in the bus stream — but treating `$D418` writes as audio-rate samples (instead of just a control output) requires extra buffering. Documented future enhancement. |

---

## 5. What stays deferred

These remain on the parking lot — none belongs in v1:

- **libsidplayfp / reSIDfp high-fidelity audio backend** as an alternative SID engine (Choice param). Pulls in GPL-2.0; needs the project-level licensing decision.
- **RSID v3 tunes** — require KERNAL ROM (`$E000-$FFFF`) and BASIC ROM (`$A000-$BFFF`). Adds ROM bundling questions (legality of Commodore ROMs is murky; the Hercules-style replacement implementations exist but are another dependency).
- **Multi-SID tunes** (second/third SID at `$D420`, `$D440`, `$D500` etc.). Extra emulator instance(s) + extra Control output groups. Doable but doubles the node's pin count.
- **Stereo SID output** (when paired with multi-SID).
- **Galway-style $D418 audio-rate sampling** — capture all volume-register writes and treat them as a 4-bit DAC sample stream into a separate Audio output port.
- **SID voice → polyphony note routing.** A "Route to NoteOn" toggle that converts gate edges into command-queue NoteOn/NoteOff messages, letting the SID drive `FVoiceAllocator`. Tempting but separate concern.
- **Filter cutoff in Hz with model-specific curves.** Requires lifting the cutoff curves out of libsidplayfp (or the original 6581/8580 calibration data); GPL-tainted unless re-derived.
- **HVSC bundled browser** — File-menu submenu listing tunes from a configured HVSC directory, mirroring the preset browser. Nice but adds UX surface.
- **Visual scope of register state** in the property panel — bar chart of all 25 registers updating live. Cool, not urgent.
- **Tap into KERNAL/BASIC IRQ handlers** for tunes that rely on them — couples to RSID support, defer with it.
- **Tempo control** — re-run the play routine at non-standard rates (half-speed, double-speed) for slow-mo / chipmunk effects. Trivial after v1; just a Float param scaling the timer rate.
- **MIDI-style clock sync** — drive the SID timer from an external Clock node so SID tunes lock to the BPM. Interesting hybrid, post-v1.
- **Recording the SID stream** to a wav file for offline use. Reaches into the audio engine; out of scope.

---

## 6. Open decisions

A few things I've called out above but want explicit before starting:

1. **`EParamKind::String` or bespoke file path on the node?** — recommend the kind. Ship in Sid.3 as a sidecar PR.
2. **Vendor floooh/chips or FetchContent?** — recommend FetchContent with a pinned commit hash. Matches existing convention.
3. **Test-fixture `.sid` files inline-as-hex or committed as binaries under `tests/fixtures/sid/`?** — recommend inline as hex-string constants, generated at test start. Keeps the repo binary-free and the test self-contained.
4. **Demo preset** — does it ship empty (with property-panel hint) or with a placeholder synthetic tune? Recommend **empty** with a doc reference; the user supplies their own .sid.

---

## 7. Sources

- [SID-PLAYER-INVESTIGATION.md](SID-PLAYER-INVESTIGATION.md) — the parent investigation
- [floooh/chips](https://github.com/floooh/chips) — Zlib chip emulator library
- [HVSC SID file format spec](https://www.hvsc.c64.org/download/C64Music/DOCUMENTS/SID_file_format.txt) — authoritative PSID/RSID format reference
- [ChiptuneSAK SID docs](https://chiptunesak.readthedocs.io/en/stable/sid.html) — secondary reference, more readable
- [High Voltage SID Collection](https://www.hvsc.c64.org/) — the canonical SID tune library
