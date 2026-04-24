#pragma once

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

		// Renders the node editor. Returns true if the graph topology changed
		// this frame (caller should recompile & publish to the audio thread).
		bool Draw(FGraphModel& Model);

		// Renders parameter sliders for the currently selected node.
		void DrawPropertyPanel(FGraphModel& Model);

	private:
		ax::NodeEditor::EditorContext* Context = nullptr;
		bool bFirstFrame = true;
	};
}
