# NodeSynth — Development Plan

A node-based software synthesizer in C++ with a Dear ImGui front-end and a node-graph editor. Built in phases, starting with a small set of nodes and growing outward.

**Target platforms:** Windows (x64) and macOS (Apple Silicon + Intel). Linux should fall out mostly for free from the library choices below, but is not a supported target.

---

## 1. Tech Stack

| Concern | Choice | Why |
| --- | --- | --- |
| Language / Std | C++20 | Concepts, `std::span`, `std::jthread`, designated initializers. Supported by MSVC 2022 and Apple Clang 15+. |
| Build | CMake 3.24+ with FetchContent | Single source of truth across Windows and macOS. Avoids vcpkg so macOS devs don't need it. |
| UI framework | Dear ImGui (docking branch) | Required. Docking branch lets us float editor / parameter panels. |
| ImGui backend | GLFW + OpenGL3 | Works on both platforms. **macOS caveat:** Apple deprecated OpenGL; we target the OpenGL 3.2 Core profile, which still runs on current macOS but will one day be removed. If/when that happens, swap the backend to Metal (`imgui_impl_metal`) — the rest of the app is unaffected. |
| Node graph editor | `thedmd/imgui-node-editor` | Most mature ImGui node lib — bezier links, selection, routing, pan/zoom. Alternative: `Nelarius/imnodes` (simpler but less featured). |
| Audio I/O | `miniaudio` (single header) | Zero-dep, cross-platform: WASAPI on Windows, CoreAudio on macOS. Same API either way. |
| MIDI I/O | `RtMidi` | Cross-platform: WinMM on Windows, CoreMIDI on macOS. |
| Serialization | `nlohmann/json` | Patch save/load. |
| Testing | Catch2 | Header-only, pleasant syntax. |

---

## 2. Architecture

### Threading model

Two threads only to begin with:

- **UI thread** — ImGui, node editor, user edits the graph, writes parameter changes.
- **Audio thread** — driven by the miniaudio callback. Reads the current graph, fills output buffers. **Must never lock, allocate, or block.**

Communication between them:

- **UI → Audio:** lock-free SPSC command queue (`moodycamel::readerwriterqueue` or a hand-rolled ring buffer). Commands are POD structs: `AddNode`, `RemoveNode`, `Connect`, `Disconnect`, `SetParam`.
- **Audio → UI:** optional SPSC queue for meters / scope data (downsampled).
- **Graph ownership:** the audio thread owns the live graph. The UI thread holds a "shadow" graph used purely for drawing and edit intent. No shared mutable state; all sync goes through the command queue.

### Platform-specific concerns

Kept behind a thin `platform/` abstraction so the rest of the code stays clean:

- **Realtime thread priority.** miniaudio's callback thread gets OS-specific priority treatment:
  - Windows: `AvSetMmThreadCharacteristics(L"Pro Audio", ...)` — the correct way to promote an audio thread. miniaudio does this for us; don't override it.
  - macOS: `thread_policy_set` with `THREAD_TIME_CONSTRAINT_POLICY` — tells the Mach scheduler this thread has hard deadlines. miniaudio handles this too.
  - Rule: don't spin up your own audio worker threads. If you do, you have to replicate the above.
- **File paths.** Use `std::filesystem` everywhere. Patch/preset directory resolves to `%APPDATA%\NodeSynth` on Windows and `~/Library/Application Support/NodeSynth` on macOS.
- **App packaging.**
  - Windows: plain `.exe` is fine during development.
  - macOS: eventually needs an `.app` bundle with `Info.plist` (required for `NSMicrophoneUsageDescription` if we ever add audio input, and for proper menu-bar behaviour). CMake's `MACOSX_BUNDLE` target property handles this. Code signing / notarization is a distribution-only concern — skip until Phase 5.
- **High-DPI / Retina.** GLFW + ImGui handle this, but we need to honour the framebuffer size (not the window size) when setting the GL viewport and to scale the ImGui font. One `platform::contentScale()` helper covers both OSes.
- **Keyboard shortcuts.** Use `Ctrl` on Windows, `Cmd` on macOS for Save/Copy/Undo. Dear ImGui exposes `io.ConfigMacOSXBehaviors` — enable it on Mac and the defaults mostly do the right thing.

### Node graph model

- Each node has typed **input ports** and **output ports**. Port types to start: `Audio` (float buffer) and `Control` (single float per block, smoothed).
- **Block-based processing.** Fixed block size (e.g. 64 or 128 samples). Each node's `process(ctx)` fills its output buffers for one block.
- **Evaluation:** topological sort on connect/disconnect, cached as a flat vector of node pointers. The audio callback walks that vector once per block. No recursion, no allocation per block.
- **Cycles:** disallow for now. Feedback/delay lines come later as explicit nodes with a 1-block latency.
- **Parameter smoothing:** every `Control` input runs through a simple one-pole smoother to avoid zipper noise on slider drags.

### Directory layout

```
NodeSynth/
├── CMakeLists.txt
├── external/            # submodules or FetchContent sources
├── src/
│   ├── main.cpp
│   ├── app/             # App shell, window, ImGui setup
│   ├── audio/           # Audio device, callback, ring buffers
│   ├── dsp/             # Node base class, concrete nodes, voice allocation
│   ├── graph/           # Graph model, topo sort, command queue
│   ├── ui/              # Node editor panels, property panels, transport
│   └── io/              # Patch serialization, MIDI
└── tests/
```

---

## 3. Phased Roadmap

Each phase ends with a runnable, hearable deliverable. No phase depends on undone work from a later phase.

### Phase 0 — Skeleton (1–2 days)

- CMake project, dependencies wired via FetchContent.
- Window opens, ImGui renders, miniaudio outputs a 440 Hz sine directly from the callback (no graph yet).
- **Verify on both Windows and macOS before moving on.** Cross-platform issues are cheapest to fix when the codebase is 300 lines, not 30,000.
- **Done when:** you hear a sine wave and can see an empty ImGui window on both OSes.

### Phase 1 — Minimal graph, three nodes

Nodes:
1. **Oscillator** (sine only) — outputs audio. Params: frequency, amplitude.
2. **Gain** — audio in, audio out. Param: gain.
3. **Output** — audio in, routes to the device. Exactly one per graph.

Features:
- Graph data model + command queue + topo sort.
- Node editor panel (add node via right-click menu, drag links, delete).
- Per-node property panel for parameter sliders.
- **Done when:** user builds `Osc → Gain → Output` in the UI and hears it.

### Phase 2 — Essential synth nodes

- **Oscillator**: add saw, square (PolyBLEP to avoid aliasing), triangle, noise.
- **ADSR envelope** — control output. Gate input.
- **Filter** — state-variable filter (LP/HP/BP outputs, resonance).
- **VCA** — audio × control.
- **MIDI Input** — outputs gate + note frequency + velocity. Single voice (monophonic) for now.
- **Done when:** user can build a subtractive mono synth and play it from a MIDI keyboard.
- **Detailed breakdown:** see [`PLAN-PHASE-2.md`](PLAN-PHASE-2.md) for sub-phase sequencing, locked decisions (Control port convention, MIDI threading), and risk list.

### Phase 3 — Polyphony & modulation

- **Voice allocator** node or global concept: one sub-graph instanced per voice, mixed at the output.
- **LFO** node.
- **Math / utility** nodes: add, multiply, scale, constant, sample-and-hold.
- Patch save/load (JSON).
- **Done when:** 8-voice polyphonic patches work, patches persist across sessions.

### Phase 4 — Depth

- **Delay** (explicit feedback node with delay line).
- **Reverb** (start with Freeverb or Schroeder).
- **Distortion / waveshaper.**
- **Sequencer** node (step sequencer with gate + CV outputs).
- **Scope / meter** nodes for debugging.
- Undo/redo in the editor.

### Phase 5 — Polish (scope-dependent)

- Presets browser.
- Multi-channel / stereo paths.
- Oversampling for nonlinear nodes.
- VST3 wrapper (optional, large).

---

## 4. Key Decisions to Lock Early

These are cheap to decide now and expensive to change later:

1. **Block size** — pick one (e.g. 64 samples) and make it compile-time. Variable block sizes complicate every node.
2. **Sample rate** — fetched from the device at startup; nodes recompute coefficients on `prepare(sampleRate)`.
3. **Port type system** — commit to `Audio` vs `Control` early. Adding a third type later (e.g. `Trigger`) is fine; retrofitting type-checking isn't.
4. **Node registration** — a central `NodeFactory` keyed by string type ID. New nodes register themselves via a static initializer. Makes serialization and the "add node" menu trivial.
5. **ID strategy** — 64-bit monotonically increasing IDs for nodes and links. Never reused. Makes the command queue and serialization safe.

---

## 5. Risks & Mitigations

| Risk | Mitigation |
| --- | --- |
| Audio glitches from UI-thread work leaking into callback | Strict rule: audio callback touches no STL containers that allocate, no mutexes. Enforce with a debug allocator hook that aborts if called from the audio thread. |
| Node editor library limitations | Prototype Phase 1 with both `imgui-node-editor` and `imnodes` if uncertain, then commit. |
| Graph cycles causing infinite loops or stack overflow | Reject connect commands that would create a cycle; detect via DFS at connect time. |
| Parameter changes causing zipper noise | One-pole smoothing on every `Control` input, not just selected ones. |
| Scope creep on nodes before the core is stable | No node past Phase 1's three until the command queue, topo sort, and property panel all work end-to-end. |
| Windows-only code sneaks in (`windows.h`, `_WIN32` assumptions, backslash paths) | Set up GitHub Actions with a `windows-latest` + `macos-latest` matrix from day one. A macOS build failure in CI costs minutes; finding it at Phase 3 costs days. |
| macOS deprecating OpenGL outright | ImGui backend swap to Metal is local to one file (`app/`). No DSP or graph code depends on the renderer. |

---

## 6. Immediate Next Steps

1. Initialize the repo (`git init`), add `.gitignore` for CMake build dirs (`build/`, `build-mac/`, `.DS_Store`, `*.user`).
2. Set up `CMakeLists.txt` with FetchContent for Dear ImGui, GLFW, miniaudio, imgui-node-editor. Verify it configures cleanly under both Visual Studio 2022 and Xcode / Ninja on macOS.
3. Add a GitHub Actions workflow with a `{windows-latest, macos-latest}` build matrix before any real code lands.
4. Land Phase 0: window + ImGui + sine-from-callback. Confirm it runs on both machines.
5. Implement the `Node` base interface and the three Phase 1 nodes in isolation (with unit tests) before wiring them into the editor.
