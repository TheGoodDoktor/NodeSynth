#pragma once

#include <string>

#include "graph/AudioCommand.h"
#include "graph/EditHistory.h"
#include "graph/Graph.h"

namespace ax::NodeEditor { struct EditorContext; }

namespace NodeSynth
{
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

		// Renders the node editor. Returns true if the graph topology changed
		// this frame (caller should recompile & publish to the audio thread).
		bool Draw(FGraphModel& Model);

		// Resets the editor's first-frame state so node positions in a freshly
		// loaded model get pushed back into the imgui-node-editor canvas.
		// Also clears the per-node drag-tracking caches: otherwise the next
		// frame's drag detector compares the new positions against stale
		// LastX/Y values, mistakes the model-driven reseed for a user drag,
		// and pushes a phantom SetNodePosition entry that drops the redo stack.
		void OnModelReplaced()
		{
			bFirstFrame = true;
			NodeDragStates.clear();
		}

		// Renders parameter sliders for the currently selected node.
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
	};
}
