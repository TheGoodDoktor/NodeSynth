#pragma once

#include <string>

#include "graph/AudioCommand.h"
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

		// Renders the node editor. Returns true if the graph topology changed
		// this frame (caller should recompile & publish to the audio thread).
		bool Draw(FGraphModel& Model);

		// Resets the editor's first-frame state so node positions in a freshly
		// loaded model get pushed back into the imgui-node-editor canvas.
		void OnModelReplaced() { bFirstFrame = true; }

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
		std::string SettingsFilePath;
		// Set when the user right-clicks a node; read by the popup body the
		// next frame to know which node the menu applies to.
		FNodeId NodeContextTarget = 0;
	};
}
