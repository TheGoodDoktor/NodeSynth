# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

NodeSynth is a standalone C++20 node-based software synthesizer with a Dear ImGui front-end. The long-form roadmap lives in `docs/PLAN.md`.

**Current status (2026-06-21):** Phases 0–5c complete (stereo + bundled presets + oversampling + Chorus + Flanger), plus the full effects roadmap (Stages E.1–E.4), Phaser, Wavetable oscillator, and the MIDI control / Modulation Matrix work. Side-features: SID Player (with browser + bundled sample tunes), MIDI Learn, project-level note input subsystem (`FMidiDeviceManager` + on-screen `FKeyboardPanel`). SPSC command queue (UI → Audio) carries `SetParam` / `NoteOn` / `NoteOff`; a parallel CC ring in `FMidiDeviceManager` feeds `FMidiCC`. Polyphonic subtractive synth target patch is the seeded default — `FVoiceAllocator` (driven by the global note-input subsystem) → `Osc` (per-voice) + `Adsr` (per-voice) → synthesised mixer → `Gain` (master trim 0.15) → `Output`. Holding 8 keys allocates 8 voices; a 9th note steals the oldest held with `Adsr` retrigger-from-current-level keeping it click-free.

Node set (~40 types):
- **Sources:** Oscillator (Sine/Saw/Square/Triangle/Noise with PolyBLEP), Wavetable Oscillator, SID Player, Constant.
- **Envelopes / modulation:** ADSR, LFO (Sine/Triangle/Saw/Square), Sample & Hold, Modulation Matrix (8×8 routing, 64 depth knobs + 8 offsets, custom grid UI).
- **Filters / EQ:** SVF (TPT/ZDF form), Equalizer (3-band), DC Blocker.
- **Dynamics:** Compressor, Limiter, Noise Gate.
- **Time / modulation FX:** Delay (2-second feedback line + Tone, true-stereo), Reverb (Freeverb, true-stereo), Chorus, Flanger, Phaser (4/6/8-stage all-pass cascade, signed feedback), Tremolo, Auto-Pan.
- **Character:** Waveshaper (TanhSoft / HardClip / SoftClip / Fold with dB Drive + Output), Bitcrusher, Ring Mod, Exciter, Stereo Widener, Haas Widener.
- **Routing / math:** Gain, VCA, Mixer, Add, Multiply, Scale.
- **Control / sequencing:** Clock (BPM gate), Sequencer (16-step external-clock-driven), Gate (manual toggle), MIDI Input, MIDI CC (smoothed Control output + Learn), Virtual Keyboard.
- **Voice:** Voice Allocator.
- **Sinks / meters:** Output, Scope (oscilloscope tap), Meter (peak + RMS dBFS tap).

Internal-only `_VoiceMixer` synthesised by the compiler when a per-voice Audio output feeds a mono Audio input. Front-end has a categorised node palette (drag-drop) with per-category coloured title bars + icons, ADSR envelope visualisation, persistent on-screen keyboard panel, File menu (New / Open / Save / Save As + Presets browser), Edit menu with Undo / Redo (`Ctrl+Z` / `Ctrl+Y` / `Ctrl+Shift+Z`, gated on `!WantTextInput`), type-in numeric entry for many params, per-voice flag toggle via right-click context menu with a `[poly]` header badge, and custom property-panel UIs (ADSR envelope plot, Sequencer 16-step grid, Scope waveform, Meter dBFS bars, Virtual Keyboard, Modulation Matrix grid, MIDI CC Learn). Patches are JSON files (`per_voice` field optional, defaults to false for back-compat); layout (`imgui.ini` + node-editor positions) persists to `~/.nodesynth/`.

Catch2 test harness (~280 `TEST_CASE`s across ~37 test files) covers the graph, smoother, every DSP node, both SPSC rings under concurrent producer/consumer, command-queue dispatch + `SetParam` fan-out to per-voice clones, patch round-trip serialisation, `INode::Clone()` for every cloneable type, the per-voice flag, voice-allocator stealing policy (oldest fully-released → tailing → held), the Compile partition with 8-voice polyphonic audio summation and independent per-voice ADSR release, every Phase 4 effect, the full effects roadmap (dynamics, EQ, tremolo/auto-pan, character set), Phaser, Wavetable, Oversampler, Modulation Matrix, MIDI CC mapping, the SID emulator + PSID loader, and undo/redo. Audio buffers are 2-channel (`float[NumOutputs][NumChannels=2][BlockSize]`) with wire-level broadcast (mono producer's L buffer aliases the consumer's R input pointer); `INode::IsOutputStereo(port)` selects L→both broadcast vs paired L→L,R→R plumbing per link; the audio callback reads both sink channels to device L+R speakers.

**Subgraphs (shipped on `feature/subgraphs`, not yet merged):** reusable `FSubgraph` nodes (palette category **Structure**) wrapping an internal graph with user-declared Audio/Control pins. Double-click to dive in (breadcrumb navigation, per-level editor contexts, `Esc` to pop); right-click → New Subgraph or `Ctrl+G` "Make Subgraph from Selection" to create one; pin-management panel (add/rename/reorder/remove) inside the dive-in editor. Compiled by **inline macro-expansion** in `FGraphModel::Compile` (a pre-pass that flattens every instance into fresh-id clones of its internals before the normal partition/DFS/plumb), so the audio path is exactly as cheap as a hand-wired equivalent and per-voice/cycle/type-checking is reused. Definitions live in a name-keyed map on `FGraphModel`, embedded in patch JSON (`subgraphs` block + `subgraph_name` on instance nodes) so patches are portable, and saved as standalone `.nspg` assets scanned from `<exe-dir>/subgraphs/` + `~/.nodesynth/subgraphs/` into a Subgraph Library panel (drag onto canvas to instance). Internal boundary node types `_SubgraphInputs` / `_SubgraphOutputs` surface the signature. Bundled `StereoFilter` asset + `Lead/Subgraph Demo` preset. Forbidden inside a subgraph: `Output`, `VoiceAllocator`. Deferred to v2: per-level undo/redo inside subgraphs, full round-trip of nested-subgraph internal links, "Reload from Asset". See `docs/PLAN-SUBGRAPHS.md`. **The backlog is now empty.**

## Build & run

Dependencies are fetched via CMake `FetchContent` — there is no vcpkg, no submodules, and no manual setup step. The first configure will clone GLFW, Dear ImGui (docking branch), miniaudio, and `thedmd/imgui-node-editor` into `build/_deps/`.

```bash
# Configure (multi-config on Windows, single-config elsewhere)
cmake -S . -B build

# Build (use --config Release on Windows/Xcode; CMAKE_BUILD_TYPE=Release is the default otherwise)
cmake --build build --config Release --parallel

# Run
./build/Release/nodesynth.exe      # Windows MSVC
./build/nodesynth                  # Ninja / Unix Makefiles
open ./build/nodesynth.app         # macOS (MACOSX_BUNDLE target)
```

CI (`.github/workflows/build.yml`) runs `{windows-latest, macos-latest}` — keep both green. There is no test target, no lint/format step, and no test framework wired up yet (the plan calls for Catch2 but it hasn't landed).

### imgui-node-editor patch

`CMakeLists.txt` rewrites `imgui_extra_math.inl` at configure time to drop a duplicate `operator*(float, ImVec2&)` that collides with Dear ImGui's own operator (enabled project-wide via `IMGUI_DEFINE_MATH_OPERATORS` in `src/NodeSynthImConfig.h`). If that file regenerates or gets clobbered, the collision comes back as a link-time or template-overload error — re-run CMake configure to reapply the patch.

## Architecture

### Two-thread model with atomic snapshot swap

The core concurrency design is the key thing to understand before touching graph or DSP code:

- **UI thread** owns `FGraphModel` (`src/graph/Graph.h`). All edits — add/remove nodes, add/remove links — mutate this model. It is never touched from the audio thread.
- **Audio thread** (miniaudio callback in `src/main.cpp`) reads a `std::shared_ptr<FAudioGraph>` held in `FAudioState::Graph` (an `std::atomic<std::shared_ptr<…>>`). The callback `load()`s the current snapshot and walks its pre-sorted `OrderedNodes`.
- **Publishing edits:** whenever `FGraphEditorPanel::Draw()` reports `bGraphChanged`, `main.cpp` calls `Model.Compile(SampleRate)` on the UI thread and `store()`s the resulting `FAudioGraph` into `AudioState.Graph`. The old snapshot's `shared_ptr` gets dropped on whichever thread releases it last — this is why node destructors must be safe to run on either thread (see the shutdown sequence in `main.cpp` which deliberately nulls the graph before ImGui teardown).
- **Command queue (UI → Audio).** A lock-free SPSC ring (`FAudioCommandRing` in `src/graph/AudioCommand.h`, 512-slot) carries `SetParam` commands from UI → audio. The audio callback drains it via `FAudioGraph::DrainCommands` at the start of each block, dispatching to nodes via an `unordered_map<FNodeId, INode*>` populated by `Compile`. UI-side widgets do a *dual write*: they call `Node->SetParamValue` directly (so sliders track immediately and `GetParamValue` stays consistent) AND push a `SetParam` command (so audio-thread state mutates in queue order alongside other commands — important for save/load replay). Structural commands (`AddNode` / `RemoveNode` / `Connect` / `Disconnect`) are **not** in the queue — those still take the snapshot-swap path. RT-safe structural mutation is a much larger engineering lift and there's no Phase 3 deliverable that needs it. `NoteOn` / `NoteOff` will land alongside polyphony.

### Real-time rules for the audio callback

The audio callback runs on miniaudio's OS-priority-elevated thread. Code reachable from `FAudioGraph::Process` or any `INode::Process` must not:
- allocate, lock, or call anything that might (no `std::vector` resize, no `std::string`, no `shared_ptr` construction/destruction).
- block on I/O or syscalls.

Parameter reads use `std::memory_order_relaxed` atomics — that's intentional; the plan's zipper-noise smoother hasn't been added yet.

### Block-based DSP

- `BlockSize` is a compile-time constant (64 samples) in `src/dsp/Node.h`. Every node processes exactly one block per `Process()` call. The audio callback in `main.cpp` chunks the device frame count into `BlockSize` windows.
- `TNodeBase<NumInputs, NumOutputs>` (in `Node.h`) is the convenience base for concrete nodes. It owns the output buffers (aligned `float[NumOutputs][BlockSize]`) and stores input pointers. **Nodes do not allocate buffers at runtime** — routing is pointer-plumbing done once at `Compile()` time.
- Unconnected inputs receive `nullptr`; each node's `Process()` must handle that case (see `FGain::Process` for the pattern).

### Graph compilation

`FGraphModel::Compile()` in `src/graph/Graph.cpp`:
1. Finds the sink by string-comparing `GetTypeName() == "Output"`. There is no central `NodeFactory` or registry yet — the sink is identified by type name, and new node types currently have to be seeded manually (see `SeedDefaultPatch` in `main.cpp`). Phase 1 accepts exactly one `FOutput`; extras are ignored.
2. Reverse-DFS from the sink produces a producers-first `OrderedNodes` vector. Nodes not reachable from the output are omitted entirely.
3. For every reachable node, clears input pointers then calls `Prepare(SampleRate)`.
4. Walks `Links` and plumbs each upstream `GetOutputBuffer()` into the downstream `SetInputBuffer()`. Buffer pointers remain valid for the lifetime of the snapshot's `shared_ptr` to each node.

Cycle prevention lives in `WouldCreateCycle` (DFS at `AddLink` time). Port type mismatches and out-of-range ports are also rejected at `AddLink`. An input port holds at most one link; connecting a second replaces the first.

### Directory layout (current)

```
src/
├── main.cpp                 # App shell: GLFW, ImGui, miniaudio, main loop, audio callback
├── NodeSynthImConfig.h      # IMGUI_USER_CONFIG (enables IMGUI_DEFINE_MATH_OPERATORS project-wide)
├── dsp/
│   ├── Node.h               # INode, TNodeBase, FProcessContext, BlockSize
│   ├── Oscillator.h         # Sine only so far
│   ├── Gain.h
│   └── Output.h             # Sink — identified by GetTypeName() == "Output"
├── graph/
│   └── Graph.{h,cpp}        # FGraphModel (UI side) + FAudioGraph (compiled snapshot)
└── ui/
    └── Editor.{h,cpp}       # imgui-node-editor panel + property panel. Pin IDs pack (NodeId, PortIndex, IsOutput) into a uint64.
```

Phase 2 added `src/midi/` for the SPSC `FMidiRing` and `src/dsp/MidiInput.{h,cpp}` for the RtMidi-backed `FMidiInput` node. Tests live under `tests/` (header-globbed into the `nodesynth_tests` Catch2 target). The `app/`, `audio/`, `io/` directories in `docs/PLAN.md` still don't exist — don't cite them as if they do.

### Phase 2 additions — quick reference

- `EParamKind` (`dsp/Node.h`): `Float` / `Choice` / `Bool`. The property panel picks `SliderFloat` / combo / checkbox accordingly. `FParamInfo::Choices` is only populated for `Choice`-kind params.
- `FOnePoleSmoother` (`dsp/Smoother.h`): applied to `FGain::Gain` and `FOscillator::Amplitude` as reference. Not applied blanket to every param — only to slider-driven values where zipper noise is audible.
- `FOscillator` ports: when a `Freq` or `Amp` Control input is connected, it overrides the param slider sample-by-sample. Disconnected → param value (smoothed for Amp).
- `FSvf` topology: linear-trapezoidal (Zavalishin/Simper TPT), `k = 2*(1 - Res)` mapping. Cutoff clamped to `[20 Hz, 0.49*SR]` every sample. Self-oscillation at Res=1 is tested for numerical stability.
- `FAdsr`: exponential approach per stage (`1 - exp(-1/tau)`), re-trigger preserves current level to avoid clicks, legato across held MIDI notes (gate stays high until all notes released).
- **MIDI threading.** `FMidiInput` owns its `RtMidiIn`. Device open/close happens on the UI thread from `SetParamValue(Param_Device, …)`. RtMidi's callback thread writes to `FMidiRing` (256-slot SPSC); `Process()` drains at block start. Events are quantized to sample 0 of the block they're drained in — sample-accurate MIDI is Phase 3+.
- **MIDI cleanup caveat.** `RtMidiIn::~RtMidiIn` joins its callback thread, so `FMidiInput::~FMidiInput` can block briefly. If the audio thread drops the last `shared_ptr` to a compiled graph that still held the MIDI node, destruction runs there and may cause an audible dropout. In practice, new-graph publishes run on the UI thread, so old graphs are typically released on UI too. Proper deferred-destroy is a Phase 3 concern.

## Deferred from Phase 1

**Structural commands through the queue.** The original plan called for `AddNode` / `RemoveNode` / `Connect` / `Disconnect` to flow through the SPSC queue too. Only `SetParam` does today; structural edits still recompile the whole `FAudioGraph` and atomic-swap the snapshot. RT-safe structural mutation needs allocation-free node construction, deferred destruction back to the UI thread, and incremental topo recomputation — substantial engineering, and no Phase 3 deliverable depends on it. Don't add new shortcuts that would make this harder to introduce later (e.g. don't bake "the audio thread can't see new nodes mid-block" assumptions outside `FAudioGraph` itself).

Resolved during Phase 3:
- `FOutput` singleton enforcement in both `AddNode` and `AddNodeWithId`.
- Editor layout persistence to `~/.nodesynth/` (`imgui.ini` + `node_editor.ini`).
- Parameter smoothing landed where it's audibly required (Gain, Oscillator amplitude, LFO rate, virtual-keyboard mod). Not blanket-applied to every Control input.

## Polyphony

The voice-allocator design is fully laid out in `docs/PLAN-PHASE-3-VOICES.md` — read that before touching `FGraphModel::Compile`'s partition algorithm or `FVoiceAllocator`. Highlights:

- **`INode::Clone()`** lives on the base class. Default implementation in `ui/NodeRegistry.cpp` instantiates a fresh node by type name and copies params by name (mirrors `LoadPatch`). Non-cloneable nodes (`FMidiInput`, `FVirtualKeyboard`, `FOutput`) override to return `nullptr`.
- **Per-voice flag** is a `bool bPerVoice` on `FNodeRecord`, toggled via `FGraphModel::SetNodePerVoice` (rejects non-cloneable types) or right-click → "Per-voice" in the editor. Patches roundtrip the flag via the optional `per_voice` JSON field.
- **`FGraphModel::Compile` partition** classifies each reachable node as Mono / PerVoice / VoiceAllocator. Per-voice nodes get cloned `MaxVoices=8` times. Validation rule: per-voice → mono Control links are rejected (return empty snapshot, log to stderr). A per-voice → mono Audio link gets a synthesised `Internal::FVoiceMixer` (one per `(FromNode, FromPort)` pair).
- **Buffer routing** has four cases: mono→mono (as before), mono→per-voice (broadcast same buffer to every clone), per-voice→per-voice (paired by voice index), per-voice→mono Audio (through the mixer).
- **`FAudioGraph::NodeById`** maps an FNodeId to `FNodeEntry { Primary; Voices }` so `DrainCommands` `SetParam` can fan out to all clones — slider drags on a per-voice node update every voice in lockstep.
- **`NoteOn` / `NoteOff` commands** broadcast to every `FVoiceAllocator` in the snapshot. UI-thread sources push through the SPSC ring; `FMidiInput` calls allocator methods directly from the audio thread (we'd otherwise need MPSC for the ring).
- **Voice stealing policy** in `FVoiceAllocator::HandleNoteOn`: same-note retrigger > oldest fully-released voice (`released_for > 100 ms`) > oldest tailing voice > steal oldest still-held. ADSR retrigger-from-current-level keeps steals click-free.
- **Polyphonic gain convention.** `Internal::FVoiceMixer` does a *straight sum* of N voice buffers — no auto-attenuation. With `NumVoices=8` and per-voice envelopes at `Sustain=0.7`, in-phase chords can peak at ~5.6 and clip at the device. Polyphonic patches must put a `Gain` (or `VCA`) downstream of the mixer and trim it: `1/N` (=0.125 for 8 voices) is clip-free; `1/√N` (~0.35) is louder with rare clips on full chords. The seeded patch uses 0.15 as a middle ground. Auto-attenuation was rejected in `docs/PLAN-PHASE-4.md` §1.9 — silently changes patch loudness on reload and breaks effect-bus expectations. Apply this rule when authoring new seeded patches or tutorials.
- **Connected Control input overrides the param.** When a node's Control input is wired (e.g. `ADSR.Env → Osc.Amplitude`), the corresponding param value is *ignored* during Process — the buffer takes over. Easy to trip on: `Osc->SetParamValue(Param_Amplitude, 0.12f)` does nothing if `Osc.Amp` is connected. To attenuate a per-voice signal, do it downstream of the per-voice mixer, not on the per-voice node's param.

## Phase 4 features

- **Effects** (`FDelay`, `FReverb`, `FWaveshaper`) all pre-allocate persistent buffers in `Prepare`; `Process` is allocation-free. Verified by tests that hold a pointer across many `Process` calls.
- **`FDelay` Time Control input** overrides the `TimeMs` param and is **NOT** smoothed (so an LFO modulating it produces clean chorus / flanger). Slider-driven param changes go through a `FOnePoleSmoother`. Test pattern: set params *before* `Prepare` so the smoother starts at the target value — otherwise tests get a 5 ms glide that smears impulse-position assertions.
- **`FSequencer`** has 64 hidden per-step params (4 fields × 16 steps), surfaced via `FParamInfo::bHidden = true`. Standard property-panel widget loop skips hidden params; save/load still round-trips them. Custom UI (`SequencerUI`) draws the 16-step grid. The pattern generalises — any node with too-many or too-niche params can hide them and surface them via a custom UI hook in `Editor.cpp`'s `DrawPropertyPanel` (see also `AdsrUI`, `ScopeUI`, `MeterUI`, `VirtualKeyboardUI`).
- **Property panel for nodes with no visible params still draws custom UIs.** `DrawPropertyPanel` shows "(no parameters)" but does *not* early-return when `Infos.empty()` or all-hidden; the custom UI dispatch (`if (auto* X = dynamic_cast<...>)`) at the bottom must always run. `FMeter` (zero params, all-custom UI) bit us here; the fix is to keep the function flowing through the dispatch.
- **`FScope` / `FMeter` are passthroughs**, not sinks — the user must wire their `Out` somewhere reaching `FOutput` for the compiler to include them in the audio path. Otherwise the meter sits there reading 0 forever.
- **Float-precision boundaries** at exact cycle wraparounds (e.g. `FClock` Phase hitting 1.0 after exactly N samples) cause edge-count tests to land at N or N+1 depending on accumulated rounding. Sequencer/clock tests use ranges (`>= 1 && <= 2`, `>= 5 && <= 12`) rather than exact counts.

## Edit history (undo/redo)

- **`FEditHistory`** in `src/graph/EditHistory.h` carries `FEditCommand` variants for `AddNode` / `RemoveNode` / `AddLink` / `RemoveLink` / `SetParam` / `SetNodePerVoice`. Linear undo + redo stacks; new user edit drops the redo stack; FIFO eviction at `MaxEntries = 200`.
- Every `FGraphModel` mutator pushes a command when `IsRecordingHistory()` is true. `FEditHistory::Apply` flips `bRecordHistory = false` during replay so undo/redo doesn't recurse.
- **`RemoveNode`** captures the full node state (type name, all params by name, `bPerVoice`, all incident links). `Undo` reconstructs everything in one shot via `AddNodeWithId` + param SetParamValues + `AddLinkWithId` per incident link. **`AddLinkWithId`** exists specifically so undo replay preserves a link's id (subsequent commands targeting that link still resolve after redo).
- **Slider-drag coalescing** in `FGraphEditorPanel::DrawPropertyPanel`: `IsItemActivated` captures the value-on-press into `ActiveParamOldValue`; `IsItemDeactivatedAfterEdit` pushes one `SetParam` history entry with old + new values. A 100-step slider drag becomes 1 undo entry. Bool checkboxes and Choice combos push instantly with old + new (single click → single edit). No-op when old == new.
- **Node positions are NOT in the history.** `imgui-node-editor` owns runtime positions; the model's `PositionX/Y` is just a seed value. Adding position-undo would require syncing back from the editor each frame. Documented as deferred.
- **Patch load and `New`** clear the history (`EditHistory.Clear()`), and the seeded `FOutput` for `New` is added with `SetRecordHistory(false)` so the singleton output isn't an undoable edit.
- **Hotkeys** in `main.cpp`: `Ctrl+Z` (or Cmd on macOS), `Ctrl+Y`, `Ctrl+Shift+Z`. All gated on `!IO.WantTextInput` so they don't fire while typing in the file-path popup.
- **`FParamInfo::bHidden`**: optional flag (default false). When true, the standard widget loop skips the param. Save/load still round-trips it. Used by `FSequencer`'s 64 per-step params.

## Coding style

All C++ in this project follows **Unreal Engine coding conventions**, applied to a standalone C++ codebase. We adopt the **style**, not the framework — see the caveat below.

### Naming

- **PascalCase** for types, variables, functions, methods, parameters, locals. No `camelCase`, no `snake_case`.
- **Type prefixes:**
  - `F` for plain structs/classes: `FAudioState`, `FNodeGraph`.
  - `E` for enums: `EOscillatorShape`.
  - `I` for interfaces / abstract bases: `INode`.
  - `T` for templates: `TRingBuffer<T>`.
  - No `U` or `A` prefixes — we are not using UObjects or AActors.
- **Booleans** prefix with `b`: `bMuted`, `bIsPlaying`.
- **Acronyms stay uppercase:** `IO`, `URL`, `MIDI`, `DSP`. So `GetID()`, not `GetId()`.

### Formatting

- **Allman braces** — opening brace on its own line for functions, classes, control blocks.
- **Tabs** for indentation (Epic standard, 4-wide display).
- **Space after control keywords:** `if (Cond)`, `for (int32_t I = 0; I < N; ++I)`.
- **Pointer/reference glued to the type:** `float* Samples`, `const std::string& Name`.
- **Always brace conditionals and loops**, even single-statement bodies.
- **`const`** on the left: `const int32_t Count` (west-const).

### Headers

- `#pragma once` (no include guards).
- Include order, each group separated by a blank line:
  1. Own header (for a `.cpp`).
  2. Project headers.
  3. Third-party headers.
  4. System / standard-library headers.
- No `using namespace` in headers.

### Important caveat — style only, not framework

This is a **standalone C++ project**, not a UE plugin or module. Do **not** use any of the following:

- UE types: `FString`, `TArray`, `TSharedPtr`, `TMap`, `int32` typedef, etc.
- UE headers: `CoreMinimal.h`, `Engine.h`.
- UE macros: `UCLASS`, `UPROPERTY`, `UFUNCTION`, `GENERATED_BODY`.

Use standard C++ instead: `std::` containers, `<cstdint>` fixed-width integers (`int32_t`, `uint32_t`), `std::string`, `std::unique_ptr` / `std::shared_ptr`. Apply Unreal naming and bracing **on top of** standard C++.

### Third-party code

External library identifiers (ImGui, GLFW, miniaudio) stay in their upstream style — we don't rename `ma_device` to `FMaDevice` or `glfwInit` to `GlfwInit`. Only our own code follows UE style.
