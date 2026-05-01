#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "graph/Graph.h"

namespace NodeSynth
{
	enum class EEditCommand : uint8_t
	{
		AddNode,           // Forward: node was added. Undo: remove it.
		RemoveNode,        // Forward: node was removed. Undo: restore node + incident links.
		AddLink,           // Forward: link was added. Undo: remove it.
		RemoveLink,        // Forward: link was removed. Undo: re-add with same id.
		SetParam,          // Forward: param changed (NewValue). Undo: restore (OldValue).
		SetNodePerVoice,   // Forward: per-voice flag toggled. Undo: restore prior flag.
		SetNodePosition,   // Forward: drag landed at NewX/Y. Undo: restore OldX/Y.
	};

	// One undoable edit. Carries enough state in its variant fields to apply
	// either direction. Bigger than a tagged-pointer design but simpler — and
	// the MaxEntries cap keeps total memory bounded.
	struct FEditCommand
	{
		EEditCommand Type = EEditCommand::SetParam;

		// Common to most.
		FNodeId NodeId = 0;
		FLinkId LinkId = 0;

		// AddNode / RemoveNode payload.
		std::string NodeType;
		float PosX = 0.0f;
		float PosY = 0.0f;
		bool bPerVoice = false;
		// Param values captured by name (matches LoadPatch ordering).
		std::vector<std::pair<std::string, float>> Params;
		// Links incident to the node — needed to restore on RemoveNode-undo.
		struct FLinkRecord
		{
			FLinkId Id;
			FNodeId FromNode;
			uint32_t FromPort;
			FNodeId ToNode;
			uint32_t ToPort;
		};
		std::vector<FLinkRecord> IncidentLinks;

		// AddLink / RemoveLink payload (also used by IncidentLinks restore).
		FNodeId FromNode = 0;
		uint32_t FromPort = 0;
		FNodeId ToNode = 0;
		uint32_t ToPort = 0;

		// SetParam payload.
		uint32_t ParamIndex = 0;
		float OldValue = 0.0f;
		float NewValue = 0.0f;

		// SetNodePerVoice payload.
		bool OldPerVoice = false;
		bool NewPerVoice = false;

		// SetNodePosition payload.
		float OldX = 0.0f, OldY = 0.0f;
		float NewX = 0.0f, NewY = 0.0f;
	};

	// Linear undo + redo stack with a fixed cap. New user edits drop the redo
	// stack. Patch load and other "system" rewrites bypass via FGraphModel's
	// SetRecordHistory(false).
	//
	// Composite groups: between Begin/EndComposite, all Push() calls accumulate
	// into a single undoable unit. Used for multi-select batch ops so a single
	// Delete-key press on N selected nodes produces 1 undo entry, not N.
	class FEditHistory
	{
	public:
		static constexpr size_t MaxEntries = 200;

		void Push(FEditCommand Cmd);  // adds to undo stack, drops redo
		bool Undo(FGraphModel& Model);
		bool Redo(FGraphModel& Model);

		// Composite/transaction support.
		void BeginComposite();
		void EndComposite();

		bool CanUndo() const { return !UndoStack.empty(); }
		bool CanRedo() const { return !RedoStack.empty(); }

		void Clear()
		{
			UndoStack.clear();
			RedoStack.clear();
			bInComposite = false;
			CurrentComposite.clear();
		}

		// Test accessors.
		size_t UndoSize() const { return UndoStack.size(); }
		size_t RedoSize() const { return RedoStack.size(); }

		// Brief one-line label for the i-th undo entry (0 = top of undo stack,
		// i.e. most recent). Used by the History panel.
		std::string GetUndoLabel(size_t Index) const;
		std::string GetRedoLabel(size_t Index) const;

	private:
		// One undoable unit. Always at least one command for a normal Push;
		// can be many for a composite group.
		using FUndoEntry = std::vector<FEditCommand>;

		// Apply Cmd to Model. bForward = true → redo, false → undo.
		void Apply(const FEditCommand& Cmd, FGraphModel& Model, bool bForward);
		// Apply an entire entry (in reverse order for undo, forward for redo).
		void ApplyEntry(const FUndoEntry& Entry, FGraphModel& Model, bool bForward);

		std::vector<FUndoEntry> UndoStack;
		std::vector<FUndoEntry> RedoStack;

		bool bInComposite = false;
		FUndoEntry CurrentComposite;
	};
}
