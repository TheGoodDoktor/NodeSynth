#pragma once

#include "graph/AudioCommand.h"
#include "graph/Graph.h"

namespace ax::NodeEditor { struct EditorContext; }

namespace NodeSynth
{
	class FGraphEditorPanel
	{
	public:
		FGraphEditorPanel();
		~FGraphEditorPanel();

		FGraphEditorPanel(const FGraphEditorPanel&) = delete;
		FGraphEditorPanel& operator=(const FGraphEditorPanel&) = delete;

		// Audio-thread command ring, used by parameter widgets to push SetParam
		// commands. Null is allowed (acts as a no-op); set it once at startup
		// from main.cpp once the audio state exists.
		void SetCommandRing(FAudioCommandRing* Ring) { CommandRing = Ring; }

		// Renders the node editor. Returns true if the graph topology changed
		// this frame (caller should recompile & publish to the audio thread).
		bool Draw(FGraphModel& Model);

		// Renders parameter sliders for the currently selected node.
		void DrawPropertyPanel(FGraphModel& Model);

		// Renders a persistent UI for any FVirtualKeyboard nodes in the graph,
		// regardless of selection. Lets the user play notes while editing other
		// nodes' parameters.
		void DrawKeyboardPanel(FGraphModel& Model);

	private:
		ax::NodeEditor::EditorContext* Context = nullptr;
		bool bFirstFrame = true;
		FAudioCommandRing* CommandRing = nullptr;
	};
}
