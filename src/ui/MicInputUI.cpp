#include "ui/MicInputUI.h"

#include <imgui.h>

#include "dsp/MicInput.h"

namespace NodeSynth
{
	void DrawMicInputUI(FMicInput& Mic)
	{
		ImGui::Separator();
		ImGui::TextDisabled("Capture Device");

		// Lazily enumerate the first time the panel is shown — keeps node
		// construction free of any audio-backend calls.
		if (!Mic.IsEnumerated())
		{
			Mic.RefreshDevices();
		}

		const std::vector<std::string>& Names = Mic.DeviceNames();
		int32_t Current = static_cast<int32_t>(Mic.GetParamValue(FMicInput::Param_Device));
		if (Current < 0 || Current >= static_cast<int32_t>(Names.size()))
		{
			Current = 0;
		}

		ImGui::SetNextItemWidth(-60.0f);
		if (ImGui::BeginCombo("##micdevice", Names[Current].c_str()))
		{
			for (int32_t I = 0; I < static_cast<int32_t>(Names.size()); ++I)
			{
				const bool bSelected = (I == Current);
				if (ImGui::Selectable(Names[I].c_str(), bSelected))
				{
					Mic.SetDevice(I);  // UI-thread device open
				}
				if (bSelected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Rescan"))
		{
			Mic.RefreshDevices();
		}

		if (Mic.IsCapturing())
		{
			ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "Capturing.");
		}
		else
		{
			ImGui::TextDisabled("Idle — select a device to start.");
		}

		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
			"Use headphones: monitoring through speakers feeds the output "
			"back into the mic and howls.");
		ImGui::PopTextWrapPos();
	}
}
