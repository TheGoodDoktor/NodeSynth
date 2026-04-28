#include "ui/ScopeUI.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <imgui.h>

#include "dsp/Scope.h"

namespace NodeSynth
{
	void DrawScopeUI(FScope& Scope)
	{
		ImGui::Separator();
		ImGui::TextDisabled("Waveform");

		const float Scale = ImGui::GetFontSize() / 13.0f;
		const float Width = std::max(280.0f * Scale, ImGui::GetContentRegionAvail().x);
		const float Height = 120.0f * Scale;

		const ImVec2 Origin = ImGui::GetCursorScreenPos();
		const ImVec2 Min = Origin;
		const ImVec2 Max(Origin.x + Width, Origin.y + Height);
		ImGui::InvisibleButton("##scope_plot", ImVec2(Width, Height));

		ImDrawList* Draw = ImGui::GetWindowDrawList();
		Draw->AddRectFilled(Min, Max, IM_COL32(20, 20, 28, 255), 3.0f);
		Draw->AddRect(Min, Max, IM_COL32(80, 80, 96, 255), 3.0f, 0, 1.0f);

		// Centre-line guide.
		const float MidY = (Min.y + Max.y) * 0.5f;
		Draw->AddLine(ImVec2(Min.x + 4.0f, MidY), ImVec2(Max.x - 4.0f, MidY),
			IM_COL32(60, 60, 80, 255), 1.0f);

		const size_t WindowSize = static_cast<size_t>(
			Scope.GetParamValue(FScope::Param_WindowSize));
		if (WindowSize < 2)
		{
			return;
		}

		// Snapshot the latest WindowSize samples.
		std::vector<float> Samples(WindowSize, 0.0f);
		Scope.Snapshot(Samples.data(), WindowSize);

		// Draw the polyline. Y maps -1..1 onto [Bottom, Top]. Outside-range
		// samples just render off-canvas (clipped by the editor).
		std::vector<ImVec2> Pts;
		Pts.reserve(WindowSize);
		const float PlotW = Max.x - Min.x - 8.0f;
		const float Amp = (Max.y - Min.y) * 0.45f;
		for (size_t I = 0; I < WindowSize; ++I)
		{
			const float T = static_cast<float>(I) / static_cast<float>(WindowSize - 1);
			const float X = Min.x + 4.0f + T * PlotW;
			const float Y = MidY - Amp * std::clamp(Samples[I], -1.0f, 1.0f);
			Pts.push_back(ImVec2(X, Y));
		}
		Draw->AddPolyline(Pts.data(), static_cast<int32_t>(Pts.size()),
			IM_COL32(180, 220, 255, 255), 0, 1.5f);
	}
}
