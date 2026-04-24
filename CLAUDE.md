# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

NodeSynth is a standalone C++20 node-based software synthesizer with a Dear ImGui front-end. The long-form roadmap lives in `docs/PLAN.md`.

**Current status (2026-04-24):** Phases 0, 1, and 2 complete — the subtractive mono synth target patch (`MIDI → Osc → SVF → VCA → Output`, with ADSR driving VCA gain and MIDI frequency driving the oscillator) is buildable in the UI. Node set: Oscillator (Sine/Saw/Square/Triangle/Noise with PolyBLEP), Gain, VCA, ADSR, SVF (TPT/ZDF form), Gate (manual toggle), MIDI Input, Output. Catch2 test harness (24 tests, 10k+ assertions) covers graph, smoother, all DSP nodes, and the SPSC MIDI ring under concurrent producer/consumer. Phase 3 (polyphony, LFO, math/utility nodes, patch save/load) is next. One Phase 1 bullet from the plan (SPSC command queue for UI→Audio graph edits) was deferred — see the deferred-work note below.

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
- **No command queue yet.** `docs/PLAN.md` describes an SPSC command queue for UI→Audio; the current implementation side-steps it by recompiling the whole graph on every edit and atomically swapping. Parameter changes flow through `std::atomic<float>` members on each node (see `FOscillator::Frequency`), not through the graph swap. **This is the one Phase 1 plan bullet deferred to later — see "Deferred from Phase 1" below.**

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

**SPSC command queue (UI → Audio).** The plan specifies a lock-free command queue carrying `AddNode` / `RemoveNode` / `Connect` / `Disconnect` / `SetParam` records. It's not built. Today, any structural edit recompiles the whole `FAudioGraph` on the UI thread and atomic-swaps the `shared_ptr` into `FAudioState::Graph`; parameter changes use per-node `std::atomic<float>`.

This works for Phase 1's scale but should land before either of:
- **Phase 3 polyphony**, where voice allocation needs fine-grained messages rather than whole-graph swaps.
- **Phase 3 patch save/load**, where atomic `SetParam` replay during deserialization will want queue semantics.

If you're adding features in between (Phase 2 nodes: saw/square/triangle/noise oscillators, ADSR, SVF filter, VCA, MIDI input), the current swap-on-edit scheme is still adequate — don't pre-build the queue speculatively, but don't add new shortcuts that would make the queue harder to introduce later either.

Minor Phase 1 polish also outstanding:
- `FOutput` is not enforced as "exactly one per graph" — extras are silently ignored by `Compile`.
- No parameter smoothing (plan flags this for `Control` inputs; no `Control`-typed inputs exist yet).
- Editor layout / node positions don't persist across runs (`ed::Config::SettingsFile = nullptr`, `imgui.ini` is gitignored).

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
