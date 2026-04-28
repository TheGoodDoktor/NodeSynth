#include "ui/MeterUI.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "dsp/Meter.h"

namespace NodeSynth
{
	namespace
	{
		// Convert a linear level to dBFS, with a floor at -60 dB.
		float LinearToDbFs(float Linear)
		{
			if (Linear < 1e-6f) { return -60.0f; }
			return 20.0f * std::log10(Linear);
		}

		// Map a dB value (range [-60, +6]) to a 0..1 bar fill fraction.
		float DbToBarFraction(float Db)
		{
			constexpr float MinDb = -60.0f;
			constexpr float MaxDb = 6.0f;
			float T = (Db - MinDb) / (MaxDb - MinDb);
			if (T < 0.0f) { T = 0.0f; }
			if (T > 1.0f) { T = 1.0f; }
			return T;
		}

		void DrawBar(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max,
			float Fraction, ImU32 BarColour)
		{
			Draw->AddRectFilled(Min, Max, IM_COL32(20, 20, 28, 255), 2.0f);
			Draw->AddRect(Min, Max, IM_COL32(80, 80, 96, 255), 2.0f, 0, 1.0f);
			const float W = Max.x - Min.x;
			const float FillW = W * Fraction;
			if (FillW > 1.0f)
			{
				Draw->AddRectFilled(Min, ImVec2(Min.x + FillW, Max.y), BarColour, 2.0f);
			}
			// 0 dB tick mark (clipping line).
			const float ZeroDbT = DbToBarFraction(0.0f);
			const float ZeroX = Min.x + ZeroDbT * W;
			Draw->AddLine(ImVec2(ZeroX, Min.y), ImVec2(ZeroX, Max.y),
				IM_COL32(255, 200, 80, 200), 1.0f);
		}
	}

	void DrawMeterUI(FMeter& Meter)
	{
		ImGui::Separator();

		const float Scale = ImGui::GetFontSize() / 13.0f;
		const float BarHeight = 14.0f * Scale;
		const float Width = std::max(260.0f * Scale, ImGui::GetContentRegionAvail().x);

		const float PeakLin = Meter.GetPeak();
		const float RmsLin = Meter.GetRms();
		const float PeakDb = LinearToDbFs(PeakLin);
		const float RmsDb = LinearToDbFs(RmsLin);

		ImDrawList* Draw = ImGui::GetWindowDrawList();

		// Peak bar.
		ImGui::TextDisabled("Peak");
		ImVec2 Origin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##peak_bar", ImVec2(Width, BarHeight));
		DrawBar(Draw, Origin, ImVec2(Origin.x + Width, Origin.y + BarHeight),
			DbToBarFraction(PeakDb), IM_COL32(255, 130, 130, 220));
		ImGui::Text("%.1f dB", PeakDb);

		// RMS bar.
		ImGui::TextDisabled("RMS");
		Origin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##rms_bar", ImVec2(Width, BarHeight));
		DrawBar(Draw, Origin, ImVec2(Origin.x + Width, Origin.y + BarHeight),
			DbToBarFraction(RmsDb), IM_COL32(120, 220, 130, 220));
		ImGui::Text("%.1f dB", RmsDb);

		// Floor / ceiling annotations.
		ImGui::TextDisabled("Range: -60 dB to +6 dB. Yellow tick = 0 dB (clipping reference).");
	}
}
