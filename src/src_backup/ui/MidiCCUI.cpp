#include "ui/MidiCCUI.h"

#include <imgui.h>

#include "dsp/MidiCC.h"

namespace NodeSynth
{
	bool DrawMidiCCUI(FMidiCC& Cc, uint64_t NodeId, uint64_t& OutLearnNodeId)
	{
		ImGui::Separator();
		ImGui::TextDisabled("MIDI CC");

		const uint8_t Raw = Cc.GetLastRaw();
		ImGui::Text("Last raw: %u", static_cast<unsigned>(Raw));

		// Visual bar for the raw 0..127 reading.
		ImGui::SameLine();
		const float Norm = static_cast<float>(Raw) / 127.0f;
		ImGui::ProgressBar(Norm, ImVec2(-FLT_MIN, 0.0f), "");

		const bool bLearning = (OutLearnNodeId == NodeId);
		if (bLearning)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 80, 60, 255));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(200, 100, 80, 255));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(160, 60, 40, 255));
		}
		bool bClicked = false;
		if (ImGui::Button(bLearning ? "Cancel Learn" : "Learn", ImVec2(-FLT_MIN, 0.0f)))
		{
			bClicked = true;
			OutLearnNodeId = bLearning ? 0 : NodeId;
		}
		if (bLearning)
		{
			ImGui::PopStyleColor(3);
			ImGui::TextDisabled("Move a controller to assign CC# and Channel.");
		}
		return bClicked;
	}
}
