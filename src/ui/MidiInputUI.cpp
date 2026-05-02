#include "ui/MidiInputUI.h"

#include <imgui.h>

#include "dsp/MidiInput.h"

namespace NodeSynth
{
	void DrawMidiInputUI(FMidiInput& Node)
	{
		const FMidiInput::FStatus S = Node.GetStatus();

		ImGui::Separator();
		ImGui::TextDisabled("MIDI Status");

		if (!S.bRtMidiAvailable)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 80, 80, 255));
			ImGui::TextWrapped("RtMidi failed to initialise. MIDI input is unavailable for this session.");
			ImGui::PopStyleColor();
			return;
		}

		if (S.Devices.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 80, 255));
			ImGui::TextWrapped("No MIDI input devices detected.");
			ImGui::PopStyleColor();
			ImGui::TextDisabled(
				"On Windows, RtMidi uses the WinMM API. Some USB MIDI 2.0\n"
				"controllers may need their vendor driver installed before\n"
				"showing up here. Check that the device works in another\n"
				"WinMM-based app (e.g. Windows MIDI test utility).");
			return;
		}

		ImGui::Text("Detected devices: %d", static_cast<int>(S.Devices.size()));
		for (size_t I = 0; I < S.Devices.size(); ++I)
		{
			const bool bOpen = (S.OpenedPort >= 0 && static_cast<size_t>(S.OpenedPort) == I);
			if (bOpen)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 220, 120, 255));
				ImGui::Text("[OPEN] %s", S.Devices[I].c_str());
				ImGui::PopStyleColor();
			}
			else
			{
				ImGui::TextDisabled("       %s", S.Devices[I].c_str());
			}
		}

		if (S.OpenedPort < 0)
		{
			ImGui::Spacing();
			ImGui::TextDisabled("Pick a device in the Device combo above.");
		}
	}
}
