#pragma once

#include <memory>
#include <string>
#include <vector>

#include "graph/AudioCommand.h"
#include "graph/EditHistory.h"
#include "graph/Graph.h"
#include "ui/KeyboardPanel.h"

namespace ax::NodeEditor { struct EditorContext; }

namespace NodeSynth
{
	class FMidiDeviceManager;
	struct FSubgraphDefinition;

	class FGraphEditorPanel
	{
	public:
		// SettingsFile: path the imgui-node-editor library writes its persistent
		// per-node positions and pan/zoom state to. Empty disables persistence.
		// The string is stored as a member because the library only takes a
		// const char* and reads it across the editor's lifetime.
		explicit FGraphEditorPanel(std::string SettingsFile = {});
		~FGraphEditorPanel();

		FGraphEditorPanel(const FGraphEditorPanel&) = delete;
		FGraphEditorPanel& operator=(const FGraphEditorPanel&) = delete;

		// Audio-thread command ring, used by parameter widgets to push SetParam
		// commands. Null is allowed (acts as a no-op); set it once at startup
		// from main.cpp once the audio state exists.
		void SetCommandRing(FAudioCommandRing* Ring) { CommandRing = Ring; }

		// Edit history for undo/redo of param edits (slider drags coalesce
		// into one entry via IsItemActivated / IsItemDeactivatedAfterEdit).
		void SetEditHistory(FEditHistory* H) { History = H; }

		// Project-level MIDI device. Replaces the FMidiInput-node-based source
		// the editor used to drain CC events from. The editor drains the
		// manager's CC ring once per frame for MIDI Learn / mapping dispatch.
		void SetMidiManager(FMidiDeviceManager* Mgr) { MidiManager = Mgr; }

		// Renders the node editor. Returns true if the graph topology changed
		// this frame (caller should recompile & publish to the audio thread).
		bool Draw(FGraphModel& Model);

		// Resets the editor's first-frame state so node positions in a freshly
		// loaded model get pushed back into the imgui-node-editor canvas.
		// Also clears the per-node drag-tracking caches (otherwise the next
		// frame's drag detector compares new positions against stale LastX/Y
		// values, mistakes the model-driven reseed for a user drag, and pushes
		// a phantom SetNodePosition entry that drops the redo stack) and pops
		// any open subgraph levels (a freshly loaded patch has different
		// subgraph definitions). Defined in Editor.cpp because it destroys
		// imgui-node-editor contexts.
		void OnModelReplaced();

		// Renders parameter sliders for the currently selected node. When dived
		// into a subgraph, the subgraph's pin-management panel is shown above
		// the selected-node properties.
		void DrawPropertyPanel(FGraphModel& Model);

		// Renders a persistent UI for any FVirtualKeyboard nodes in the graph,
		// regardless of selection. Lets the user play notes while editing other
		// nodes' parameters.
		void DrawKeyboardPanel(FGraphModel& Model);

		// Renders the patch metadata editor (name, author, BPM, notes).
		void DrawPatchInfoPanel(FGraphModel& Model);

		// Renders a list of recent edit-history entries, most-recent first.
		// Click an entry to jump there. Doesn't apply the jump itself —
		// records it on the panel as a pending count; main.cpp polls and
		// applies through the regular Undo/Redo flow (including recompile).
		void DrawHistoryPanel(FGraphModel& Model);

		// Lists all current MIDI controller bindings with a remove button per
		// entry. Each row shows channel + CC + target (NodeType + param name).
		// Click [x] to unmap; goes through the same undoable path as
		// right-click → "Unmap MIDI" on the param widget.
		void DrawMidiMappingsPanel(FGraphModel& Model);

		// Pending history jump count (positive = number of Undos to apply,
		// negative = number of Redos). Read + cleared by main.cpp.
		int32_t TakePendingHistoryJump()
		{
			const int32_t V = PendingHistoryJump;
			PendingHistoryJump = 0;
			return V;
		}

	private:
		// One open subgraph nesting level. The base (patch) level uses Context
		// below; each dive pushes a level with its own imgui-node-editor context
		// (so pan / zoom / selection are per-level) editing a definition's
		// internal graph. Heap-allocated via unique_ptr so a vector realloc
		// never invalidates the context / model pointers handed to callers.
		struct FSubgraphLevel
		{
			ax::NodeEditor::EditorContext* Context = nullptr;
			FGraphModel* Model = nullptr;                       // → Definition->InternalGraph
			std::shared_ptr<FSubgraphDefinition> Definition;   // keeps it alive
			std::string Name;
		};
		std::vector<std::unique_ptr<FSubgraphLevel>> SubgraphStack;

		// Pushes a new level editing Def's internal graph. Reset deferred nav
		// state is applied at the end of Draw so it never mutates the stack
		// mid-frame.
		void EnterSubgraph(const std::shared_ptr<FSubgraphDefinition>& Def);
		// Destroys subgraph levels until exactly KeepDepth remain (0 = patch).
		void PopToLevel(int32_t KeepDepth);

		// Ctrl+G: wraps the current node selection into a new subgraph instance.
		// Links crossing the selection boundary are auto-promoted to pins
		// (incoming → input pins, outgoing → output pins, deduped by source
		// port). Forbidden nodes (Output / VoiceAllocator / boundaries) are
		// dropped from the selection. Base level only; not undoable in v1.
		// Returns true if it created a subgraph (caller should recompile).
		bool MakeSubgraphFromSelection(FGraphModel& PatchModel);

		// Pin-management UI for the open subgraph definition: add / rename /
		// reorder / remove input + output pins. Edits propagate to the boundary
		// nodes and to every instance (in PatchModel and any open parent level)
		// so positional port links stay consistent. Sets bSubgraphParamDirty on
		// a structural change so the patch recompiles.
		void DrawSubgraphPinPanel(FGraphModel& PatchModel, FSubgraphDefinition& Def);

		// "Add pin" type selectors (0 = Audio, 1 = Control) for the pin panel.
		int32_t AddInputPinType = 0;
		int32_t AddOutputPinType = 0;
		// Deferred pin removal (applied after the pin lists draw, so the pin
		// vector isn't mutated mid-iteration). A removal with connected links
		// routes through a confirmation modal first.
		bool bConfirmPinRemoveIsInput = false;
		int32_t ConfirmPinRemoveIndex = -1;
		uint32_t ConfirmPinRemoveLinkCount = 0;
		bool bPinRemoveConfirmed = false;

		// Deferred navigation, captured during Draw and applied after the
		// editor canvas closes. PendingDive non-null = dive into it next; a
		// non-negative PendingPopTo = pop to that depth. -1 = no pop pending.
		std::shared_ptr<FSubgraphDefinition> PendingDive;
		int32_t PendingPopTo = -1;

		// Serial for naming new subgraphs created from the editor ("Subgraph 1",
		// "Subgraph 2", …). Unique names keep the recursion check unambiguous.
		int32_t NextSubgraphSerial = 1;

		// Set when a param is edited inside a subgraph. Unlike base-level edits
		// (whose SetParam command reaches the audio node directly), a subgraph
		// internal node only exists in the audio graph as a compiled clone with
		// a different id, so the queued command is dropped — the change applies
		// on recompile. The flag folds into the next Draw's "graph changed"
		// return so main.cpp recompiles the patch (re-expanding the edited
		// definition). One-frame latency; cheaper than recompiling every frame.
		bool bSubgraphParamDirty = false;

		ax::NodeEditor::EditorContext* Context = nullptr;
		bool bFirstFrame = true;
		FAudioCommandRing* CommandRing = nullptr;
		std::string SettingsFilePath;
		// Set when the user right-clicks a node; read by the popup body the
		// next frame to know which node the menu applies to.
		FNodeId NodeContextTarget = 0;

		// Edit history (optional). Slider edits coalesce: when a widget becomes
		// active we capture its value-on-press; when it deactivates after edit
		// we push a SetParam edit-history entry with old + new values.
		FEditHistory* History = nullptr;

		// Global MIDI device manager. CC events for MIDI Learn come from here.
		FMidiDeviceManager* MidiManager = nullptr;

		// On-screen keyboard panel. Owns its own state (held notes, octave,
		// velocity); pushes Note On/Off through the command sink.
		FKeyboardPanel Keyboard;
		FNodeId   ActiveParamNode = 0;
		uint32_t  ActiveParamIndex = 0;
		float     ActiveParamOldValue = 0.0f;
		bool      bActiveParamCaptured = false;

		// Per-node drag tracking for position undo/redo. Updated each frame
		// in Draw — we compare the editor's current position against the
		// previous frame's to detect drag start/end.
		struct FNodeDragState
		{
			float LastX = 0.0f, LastY = 0.0f;
			float DragStartX = 0.0f, DragStartY = 0.0f;
			bool bDragging = false;
			bool bSeen = false;  // true if we've sampled this node at least once
		};
		std::unordered_map<FNodeId, FNodeDragState> NodeDragStates;

		// History panel jump request. Positive = Undo N times, negative = Redo.
		// Main.cpp polls via TakePendingHistoryJump.
		int32_t PendingHistoryJump = 0;

		// MIDI Learn state. When LearnTargetNodeId != 0, the editor is in
		// learn mode for the named (NodeId, ParamIndex) target — the next
		// CC event past the start-time guard window establishes the binding.
		// Esc clears the state.
		FNodeId  LearnTargetNodeId = 0;
		uint32_t LearnTargetParamIndex = 0;
		double   LearnStartTimeSeconds = 0.0;

		// Sentinel value stored in LearnTargetParamIndex when the learn
		// target is an FMidiCC node (assigning its Cc / Channel params)
		// rather than the standard "bind this param to that CC" mapping.
		static constexpr uint32_t LearnSentinel_MidiCcNode = 0xFFFFFFFFu;
	};
}
