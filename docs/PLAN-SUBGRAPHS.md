# NodeSynth — Subgraphs Plan

A new `FSubgraph` node that wraps an internal graph and presents user-defined input / output pins to the outside. The internal graph is editable in-place via a dive-into navigation model; subgraph definitions can be saved to disk as reusable assets. Mental model: Unreal Blueprint *Functions* — declared signature on the outside, callable graph on the inside, sharable across patches.

**Entry state:** Phases 0–5c done, wavetables shipped, 30 bundled presets. The graph compiler (`FGraphModel::Compile`) does reverse-DFS from the `FOutput` sink, partitions Mono / PerVoice / VoiceAlloc, plumbs buffer pointers, and returns an allocation-free `FAudioGraph` snapshot. Patches are flat JSON: nodes + links + metadata, with per-voice flags. There is no concept of nested graphs yet.

**Goal:** drop a `Subgraph` node into a patch, dive into it, lay out an internal graph that has user-declared pins, exit back to the parent. The subgraph behaves identically to a hand-wired equivalent at runtime. Save it to a `.nspg` file; drag a fresh instance from the library into another patch and have it work.

**Final deliverable that defines "done":**
- `FSubgraph` registered under a new **Structure** category.
- Double-clicking a subgraph node enters its internal editor (or via "Edit" button on the property panel). A breadcrumb above the canvas shows the navigation path; clicking it returns to the parent.
- The subgraph editor shows two special boundary nodes (`Inputs` and `Outputs`) that surface the subgraph's signature. Adding a pin via the side panel makes a new port appear on both the boundary node and the subgraph instance in the parent.
- A patch with one subgraph compiles to bit-identical output as the inlined equivalent (audited by a round-trip test).
- Subgraphs save to `<exe-dir>/subgraphs/<Name>.nspg` and a Library panel lets the user drag instances into their patch.
- One bundled subgraph: `Stereo Filter` (SVF + per-voice glide, packaged) — the first reusable building block.

---

## 1. Decisions to lock first

### 1.1 Compile model: inline expansion

**Decision: subgraphs are macro-expanded at `FGraphModel::Compile` time. The audio graph never sees the subgraph node.**

Algorithm sketch:

```
For each FSubgraph node S in the parent graph:
    Make fresh-id clones of all of S's internal nodes.
    Make fresh-id clones of all internal links between non-boundary nodes.
    For each external link (Producer → S.Input_K):
        Find every internal link (InputBoundary.Output_K → Consumer.Port).
        Rewire: link Producer directly to Consumer.Port.
    For each external link (S.Output_K → Consumer):
        Find the unique internal link (Producer.Port → OutputBoundary.Input_K).
        Rewire: link Producer.Port directly to Consumer.
    Discard S, the boundary nodes, and all incident links.
```

After expansion, `Compile` proceeds exactly as today — partition + DFS + plumb. There is no runtime overhead, no nested `FAudioGraph`, no extra block-rate dispatch. Mips of behaviour come for free: per-voice cloning, link-type checks, cycle detection all operate on the expanded graph.

**Trade-off vs. nested-graph runtime:** a per-instance `FAudioGraph` per subgraph would give cleaner per-instance state encapsulation (each subgraph instance is a black box at runtime) but requires sub-block dispatch and per-instance buffers. For the level of complexity NodeSynth uses, inlining is strictly simpler and zero-cost. If profile data later shows we need compilation caching for big subgraphs, that's a follow-on optimisation, not a re-architecture.

### 1.2 Asset storage: separate files, patches inline definitions by name

**Decision: subgraph assets live in `subgraphs/` as standalone `.nspg` files; patches embed a copy of every referenced definition keyed by name.**

Reasoning:
- **Standalone assets** make subgraphs reusable across patches and shareable as files.
- **Patch-embedded copies** keep patches portable — a friend opens your patch on their machine and it just works, even if they don't have the source `.nspg`.
- The trade-off: editing the asset doesn't auto-propagate to existing patches. Acceptable for v1 — the tooling can offer a "Reload from Asset" action per instance to opt into propagation, and a "Save back to Asset" action to push changes the other way.

Within a single patch, all instances of subgraph `"StereoFilter"` share one definition entry. Two instances of the same subgraph in the same patch get the same internals every Compile expansion. If the user wants two divergent variants in one patch, they save under different names.

### 1.3 Boundary nodes: two special types, one of each per subgraph

**Decision: introduce `FSubgraphInputs` and `FSubgraphOutputs` as internal-only node types.**

- `FSubgraphInputs` has zero input ports and N output ports. Each output corresponds to one declared pin of the subgraph; the value at that output is whatever the parent graph wired into the subgraph instance's input N.
- `FSubgraphOutputs` has zero output ports and N input ports. Whatever's wired into input N becomes the value the subgraph instance presents at its output port N.

Both are singletons within a subgraph (exactly one of each, auto-created when a new subgraph is opened, and not deletable from the editor). Mirror the existing `FOutput` singleton enforcement.

Like `Internal::FVoiceMixer`, these node types live in `src/dsp/internal/` and are not registered in the palette — they appear only inside subgraph editors.

### 1.4 Pin schema

Each declared pin carries:

```cpp
struct FSubgraphPin
{
    std::string Name;          // Display name on the port
    EPortType Type;            // Audio or Control
    std::string Description;   // Optional tooltip
    // (No default value param in v1 — mirrors FOscillator's "disconnected
    // input reads as 0/nullptr" convention. v2 could add a per-pin default.)
};
```

Pins are stored as `std::vector<FSubgraphPin> InputPins` / `OutputPins` on the subgraph definition. Order of pins is preserved on save/load and matches port indices.

### 1.5 Editor UX: dive-into navigation, single canvas

**Decision: one node-editor canvas, a stack of "currently open" subgraph definitions, breadcrumb at the top.**

Inspired by Houdini's network-dive model rather than Blueprint's tab model, because:
- ImGui-node-editor instances are heavyweight; running multiple in parallel is messy.
- Single-canvas + breadcrumb is just as expressive at NodeSynth's scale (we don't expect 12 simultaneously open subgraphs).

Flow:
- The user adds a `Subgraph` node and double-clicks it (or hits an "Edit" button in the property panel). The canvas swaps to render the internal graph; a breadcrumb at the top reads `Patch ▸ Subgraph: <Name>`.
- The breadcrumb is clickable — clicking the parent crumb returns to the parent graph; clicking a deeper crumb dives further.
- All canvas state (zoom, pan, selection) is per-level. Stored in `FGraphEditorPanel` as a stack alongside the current model pointer.
- New nodes / links / params edited inside the subgraph go into the subgraph's `FGraphModel` instance, not the parent's.

### 1.6 Per-voice flag

**Decision: flag is on the `FSubgraph` *instance*; internals inherit it at expansion time.**

Marking a subgraph instance per-voice makes Compile clone every internal node per voice. The internal graph itself doesn't carry per-voice flags on its nodes — it's polymorphic in that dimension. (If an internal node *can't* be per-voice — e.g. `FMidiInput` — its `Clone()` returns nullptr and `SetNodePerVoice` rejects with a compile error on the *instance*.)

This matches user expectation: a `Stereo Filter` subgraph used in a polyphonic patch becomes per-voice; in a mono FX bus it stays mono. Same definition, different instantiations.

Caveat: subgraphs can't contain `FOutput`, `FVoiceAllocator`, `FMidiInput`, or `FVirtualKeyboard`. The first because `FOutput` is the patch-level sink; the others because they're effectively singletons. Compile rejects any subgraph containing these with a clear error.

### 1.7 Recursion / nesting

**Decision: subgraphs can contain other subgraphs; a subgraph cannot contain itself directly or transitively.**

Cycle detection at Compile expansion: keep a depth-first recursion stack of definition names; reject if a subgraph name is already in the stack.

Recursion depth is bounded by the asset graph (the user can't infinite-loop short of editing JSON by hand). Practically a depth limit of 16 keeps Compile time bounded; reject with a clear error past that.

### 1.8 Multiple instances of the same subgraph

**Decision: instances share a single definition entry in the parent patch; expansion produces independent fresh-id clones each time.**

Means:
- One source of truth for the definition per patch — edit it once, every instance sees the change.
- Each instance has independent runtime state (per-voice clones are per-instance).

Cross-patch instances are independent — pasting a subgraph into another patch copies the definition over. From then on the two patches' definitions diverge.

### 1.9 Save / load

Two surfaces:

1. **Patch JSON** gains a top-level `"subgraphs"` object — name → definition. Each definition is a self-contained JSON sub-object holding pins + internal nodes + internal links. Subgraph instances reference by name in the existing `nodes` array via a `subgraph_name` field.
2. **Standalone `.nspg` files** in `subgraphs/`. The format is just one definition serialised the same way as a patch JSON's `subgraphs[Name]` entry — easy to copy in/out of a patch.

`PatchSerializer` extends to handle both. Existing patches without a `subgraphs` section round-trip unchanged.

### 1.10 Library scanning and drag-drop

**Decision: mirror the bundled-presets / bundled-wavetables pattern.**

- `<exe-dir>/subgraphs/` for bundled, `~/.nodesynth/subgraphs/` for user-installed.
- A new `FSubgraphLibrary` browser scans both at startup.
- A "Subgraph Library" panel (small, dockable) shows the available `.nspg` files; the user drags one onto the canvas to insert a new instance with that definition.
- The drag payload mirrors the node-palette `PaletteDragPayloadId` pattern: a small ID that the canvas drop target resolves to "create instance + embed definition".

### 1.11 Naming and palette location

- **Node type name**: `Subgraph`.
- **Display label in palette**: `Subgraph` under a new category `Structure`.
- **Asset extension**: `.nspg` ("NodeSynth subgraph"). JSON content. Not gzipped — diffability over file size.
- **Boundary node type names**: `_SubgraphInputs` and `_SubgraphOutputs`. Underscore prefix matches the existing `_VoiceMixer` convention for compiler-synthesised internal types.

### 1.12 Edit history

**Decision: one history stack per `FGraphModel`. The patch model and each subgraph definition each have their own.**

Undo/redo within a subgraph editor only undoes operations on that subgraph; popping back to the parent uses the parent's history. Cross-level operations (e.g. "add subgraph instance to parent") live in the parent's history because that's where the model edit happens.

This avoids cross-level confusion about what an undo means and matches Blueprint's per-graph undo isolation.

---

## 2. Sub-phases

```
SG.1 (FSubgraphDefinition + JSON serialization)         ──┐
SG.2 (boundary nodes + Compile-time expansion)          ──┤
SG.3 (dive-in editor + breadcrumb)                      ──┼── ship
SG.4 (pin management UI + per-voice flag plumbing)      ──┤
SG.5 (asset library + drag-drop + Save / Load As)       ──┤
SG.6 (showcase subgraph + polish)                       ──┘
```

Six phases, ~2–3 weeks total. Each phase is independently shippable — graph compiles + tests pass at each milestone.

### SG.1 — Definition + serialization (~1 day)

`src/graph/SubgraphDefinition.h`:

```cpp
struct FSubgraphPin { std::string Name; EPortType Type; std::string Description; };

struct FSubgraphDefinition
{
    std::string Name;
    std::vector<FSubgraphPin> InputPins;
    std::vector<FSubgraphPin> OutputPins;
    FGraphModel InternalGraph;   // owns its own nodes / links / metadata
};
```

`src/io/SubgraphSerializer.{h,cpp}` — converts `FSubgraphDefinition` to/from JSON, mirroring the existing patch serializer (much of the per-node and per-link code can be shared). `PatchSerializer` gains a `subgraphs` block and a `subgraph_name` field on subgraph nodes.

**Done when:**
- `FSubgraphDefinition` round-trips through JSON via test-driven serialization.
- An existing patch without subgraphs round-trips unchanged (back-compat regression test).

### SG.2 — Boundary nodes + Compile expansion (~2 days)

`src/dsp/internal/SubgraphBoundary.h`:

```cpp
class FSubgraphInputs  : public INode { /* dynamic NumOutputs */ };
class FSubgraphOutputs : public INode { /* dynamic NumInputs  */ };
```

These don't use `TNodeBase` (which has compile-time-fixed port counts). They allocate their input/output buffer arrays from the subgraph definition's pin count when constructed.

`FSubgraph` user-facing node: stores a definition pointer (looked up by name from the patch's subgraph map), plus the per-voice flag. Forward dynamic input/output ports based on the definition's pins.

The expansion algorithm in §1.1 lives in `FGraphModel::Compile` as a pre-pass before the existing partition logic. It rewrites a working copy of the model rather than mutating the user's `FGraphModel` directly — pre-pass leaves the user-visible model untouched.

**Done when:**
- A simple subgraph (one Audio in, one Audio out, an `FOscillator` inside connected boundary-to-boundary) produces audio identical to the unwrapped equivalent.
- Compile rejects subgraphs containing forbidden node types (`FOutput`, `FMidiInput`, `FVirtualKeyboard`, `FVoiceAllocator`) with a structured error message.
- Recursion cycle detection rejects a subgraph that transitively references itself.

### SG.3 — Dive-in editor + breadcrumb (~3 days)

`FGraphEditorPanel` gains a stack of `FEditorContext` entries. Each entry holds:
- A pointer to an `FGraphModel` (parent or subgraph)
- That level's editor-state (zoom, pan, selection)
- Breadcrumb display name

A breadcrumb bar at the top of the canvas renders the stack. Clicking a crumb pops back. Double-clicking an `FSubgraph` node pushes its definition's `FGraphModel`.

When in a subgraph context:
- The `FSubgraphInputs` and `FSubgraphOutputs` nodes appear as fixed-position decorative nodes (left and right edges of the canvas).
- The standard "Add Node" palette is filtered to exclude the forbidden internal types.

**Done when:**
- Double-click on a subgraph node opens its internals.
- Breadcrumb navigates back via click.
- New nodes / links inside the subgraph affect the subgraph's model, not the parent's.
- Selection state restored when bouncing back and forth between levels.

### SG.4 — Pin management UI (~1 day)

A side panel inside the subgraph editor lists pins:

```
Inputs:
  + Add input
  Audio  "Audio"      [↑][↓][⨯]
  Control "Cutoff"    [↑][↓][⨯]

Outputs:
  + Add output
  Audio  "Out"        [↑][↓][⨯]
```

Adding / removing / renaming / reordering pins mutates `InputPins` / `OutputPins` on the definition. Each change re-renders the `FSubgraphInputs` / `FSubgraphOutputs` boundary nodes (port count and labels), and broadcasts to every `FSubgraph` instance referencing this definition (via the patch-level definition map) so the external pins update too.

Removing a pin orphans any internal links to/from that boundary port. Show a confirmation if there are connected links.

Per-voice flag: standard right-click → Per-voice on the instance, same UX as any other node.

**Done when:**
- Adding / renaming / removing pins works smoothly.
- All instances of a subgraph reflect pin changes immediately.
- Removing a pin warns about live link breakage.

### SG.5 — Asset library + drag-drop (~2 days)

`src/io/SubgraphBrowser.{h,cpp}` — analog of `WavetableBrowser`. Scans `<exe-dir>/subgraphs/` and `~/.nodesynth/subgraphs/`. User entries shadow bundled by display name.

Library panel (`SubgraphLibraryPanel.cpp`) rendered as a dockable ImGui window. Two-pane: bundled / user. Each entry is a draggable button. Drop on the canvas creates a fresh `FSubgraph` instance, copies the definition into the patch's subgraph map, and instantiates the node.

File menu: "Subgraph → Save As..." opens a save dialog scoped to `subgraphs/`. "Subgraph → Reload from Asset" pulls the latest `.nspg` over the in-patch definition.

**Done when:**
- Drag from library panel onto canvas creates a working subgraph instance.
- Save As writes a `.nspg` that the library panel picks up after Refresh.
- A patch saved with a subgraph reopens cleanly on a machine without the source `.nspg` (definition is embedded).

### SG.6 — Showcase + polish (~1 day)

Bundle one subgraph as proof-of-concept: `Stereo Filter` —
- Inputs: `Audio` (Audio), `Cutoff` (Control), `Resonance` (Control)
- Output: `Audio` (Audio)
- Internals: `FSvf` with both Cutoff and Resonance Control inputs wired through, output goes to the boundary's Audio out.

Add to a new showcase preset: `Lead/Subgraph Demo` — uses two instances of `StereoFilter` in series with different cutoff envelopes.

Polish:
- "Make Subgraph from Selection" — select N nodes in the parent graph, hit `Ctrl+G`, get a new subgraph instance with the selected nodes inside (dangling links auto-promoted to pins).
- Keyboard shortcut: `Esc` to pop one breadcrumb level (matches Houdini's "U" / Blueprint's `Tab` UX).

**Done when:**
- The bundled `StereoFilter.nspg` ships with the binary.
- The showcase preset loads and plays.
- "Make Subgraph from Selection" works on a 3-node selection round-trip.

---

## 3. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Compile expansion produces ID collisions when the parent's IDs and the cloned-subgraph IDs overlap | The expansion pass allocates fresh IDs from a local counter, never re-using parent IDs. Tested directly. |
| Per-voice flag interactions: a subgraph contains another subgraph and the outer is per-voice — internals of the inner cascade get cloned eight times each | Expected and correct — per-voice is transitive. Tests cover the nested case. CPU cost scales linearly with voice count, same as a hand-wired equivalent. |
| User saves a subgraph asset, then deletes the `.nspg`. Their patch still has the embedded copy and works fine. They later expect "Reload from Asset" to silently update — but the asset is gone | "Reload from Asset" surfaces a clear error and leaves the embedded definition intact. No silent loss. |
| Renaming a subgraph in the patch breaks the name-based references | All instances reference by name; renaming updates every instance's `subgraph_name` field atomically (UI thread only). |
| Big subgraph asset bloats the patch JSON (many instances × full definition is fine, since we share by name; but the definition itself can still be heavy) | Acceptable for v1. JSON compression is a separate concern that would benefit any large patch, not just subgraphs. |
| Pin add / remove during live audio playback could leave a half-rebuilt port array racing with `Process` | Pin edits go through the existing UI-thread path that triggers a `Compile` + atomic snapshot swap. The audio thread sees one or the other, never a half-built definition. Same convention as every other graph edit. |
| User attempts to put `FOutput` inside a subgraph — confused error about audio not playing | Compile rejects with a structured message: *"Subgraph 'X' contains forbidden node type 'Output'. Subgraphs can't contain the patch sink."* Surface in the same red banner area as existing compile errors. |
| Subgraph editor's breadcrumb gets very deep (5+ levels) and the user gets lost | Soft-limit nesting depth at 8 levels for v1; Compile rejects past that. Breadcrumb width truncates from the left with ellipsis when it overflows the canvas. |
| `Make Subgraph from Selection` on a non-contiguous selection (links cross the boundary in messy ways) produces a confusing pin layout | Heuristic: every dangling link becomes a pin named after the source/destination port. User can rename in the pin panel. Document the heuristic. |

---

## 4. What stays deferred

- **Live binding to the asset.** v1 embeds a copy in the patch; no auto-sync from `.nspg` updates. v2 could expose a "linked instance" mode that re-pulls on every patch open.
- **Per-pin default values** for unconnected inputs. Currently disconnected → 0/nullptr (matches `FOscillator`). A per-pin `DefaultValue` (Float) and `DefaultBuffer` (silent) would let subgraph authors give reasonable defaults; not required for v1.
- **Cross-asset versioning.** No `version` field on `.nspg` files yet. When schema evolves, breaking changes will need a migration shim.
- **Tabbed editor** (Blueprint-style). Single-canvas + breadcrumb is enough for now; tabbed editing is a UI-framework lift if the user finds breadcrumb frustrating.
- **Subgraph parameter inputs** (a `Float` param on the subgraph instance that's exposed as a parameter slider, not a pin). v1 has only Control pins; the user can wire a `Constant` upstream of an input pin to fake this.
- **Local-variable / pin-passthrough nodes inside a subgraph.** Useful for grouping wiring but not foundational; can be added later.
- **Per-instance overrides** (each instance carries a divergent edit on top of the shared definition). Adds complexity; unclear demand.
- **Subgraph diffing / merge** for collaborative workflows. Far future.
- **Inline preview** of a subgraph's contents on hover or as a node-body thumbnail. Nice but expensive (each thumbnail needs a mini-renderer).
- **Compile cache** keyed by `(definition hash, per-voice flag)`. Optimisation; only matters once subgraphs become large enough to slow down `Compile`.

---

## 5. Why this is bigger than wavetables

Wavetables took ~1 week because the storage shape was simple (one binary asset format), the runtime path was a single new node, and the UI was one custom property panel. Subgraphs touch:

- A new **graph-model concept** (nested `FGraphModel` instances) that crosses the patch / model / compile boundary.
- A new **editor navigation model** (breadcrumb + dive-in) that interacts with imgui-node-editor's internal state (zoom, pan, selection).
- A new **asset format** with cross-patch versioning concerns.
- A new **boundary node primitive** with dynamic port counts — the first node type whose port count isn't known at compile time.
- A non-trivial **Compile-time expansion algorithm** that has to handle ID collisions, per-voice transitivity, recursion, and forbidden-node-type rejection.
- A new **palette category** ("Structure") and a new **library panel** alongside Presets and Wavetables.

Realistic estimate: **~2–3 weeks** for v1 as scoped. The phase split lets each SG.X land independently — graph compiles, tests pass, the user gets incremental value at every milestone (e.g. SG.3 produces a usable subgraph editor before the asset library and Save As exist).

The architectural bet is on **inline expansion**: by treating the subgraph as macro-time syntactic sugar rather than a runtime container, the audio path stays exactly as cheap as today's flat graph, and per-voice / link-type / cycle-detection logic gets reused rather than reinvented. The cost is paying for the expansion pass at every Compile — fine, since compiles are rare (per graph edit) and the work is linear in node count.
