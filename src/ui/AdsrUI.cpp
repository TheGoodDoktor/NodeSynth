#include "ui/AdsrUI.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "dsp/Adsr.h"

namespace NodeSynth
{
	namespace
	{
		// Standard ADSR-diagram convention: each stage's curve reaches its target
		// by the end of its labelled duration. We use exponential-approach shapes
		// scaled with a fixed factor so they visually arrive at the target rather
		// than the ~63% they'd hit at one time constant. Five time constants is
		// enough to land within ~1%.
		constexpr float ApproachFactor = 5.0f;

		float AttackCurve(float Normalized)
		{
			// 0..1 → exp approach from 0 toward 1.
			return 1.0f - std::exp(-ApproachFactor * Normalized);
		}

		float DecayCurve(float Normalized, float SustainLevel)
		{
			// 0..1 → exp approach from 1 toward sustain.
			const float Remaining = 1.0f - SustainLevel;
			return 1.0f - Remaining * (1.0f - std::exp(-ApproachFactor * Normalized));
		}

		float ReleaseCurve(float Normalized, float StartLevel)
		{
			// 0..1 → exp approach from start toward 0.
			return StartLevel * std::exp(-ApproachFactor * Normalized);
		}
	}

	void DrawAdsrUI(FAdsr& Adsr)
	{
		const float AttackMs = Adsr.GetParamValue(FAdsr::Param_AttackMs);
		const float DecayMs = Adsr.GetParamValue(FAdsr::Param_DecayMs);
		const float Sustain = Adsr.GetParamValue(FAdsr::Param_Sustain);
		const float ReleaseMs = Adsr.GetParamValue(FAdsr::Param_ReleaseMs);

		ImGui::Separator();
		ImGui::TextDisabled("Envelope");

		// -- Lay out a fixed-aspect plot area -----------------------------------
		// Sizes scale with the font so the plot grows on hi-DPI displays alongside
		// everything else. (GetFontSize() / 13 = the DPI scale set in main.cpp.)
		const float Scale = ImGui::GetFontSize() / 13.0f;
		const float Width = std::max(220.0f * Scale, ImGui::GetContentRegionAvail().x);
		const float Height = 120.0f * Scale;
		const ImVec2 Origin = ImGui::GetCursorScreenPos();
		const ImVec2 Min = Origin;
		const ImVec2 Max(Origin.x + Width, Origin.y + Height);

		ImGui::InvisibleButton("##adsr_plot", ImVec2(Width, Height));

		ImDrawList* Draw = ImGui::GetWindowDrawList();
		Draw->AddRectFilled(Min, Max, IM_COL32(20, 20, 28, 255), 3.0f);
		Draw->AddRect(Min, Max, IM_COL32(80, 80, 96, 255), 3.0f, 0, 1.0f);

		// Horizontal grid line at sustain level (subtle).
		const float SustainY = Max.y - Sustain * (Max.y - Min.y);
		Draw->AddLine(ImVec2(Min.x + 4.0f, SustainY), ImVec2(Max.x - 4.0f, SustainY),
			IM_COL32(60, 60, 80, 255), 1.0f);

		// -- Allocate horizontal space to each stage ----------------------------
		// A:D:R get widths proportional to their times. Sustain is a fixed slab.
		// Floors prevent very-short stages from disappearing entirely.
		const float TimeTotal = AttackMs + DecayMs + ReleaseMs;
		const float SustainPxFraction = 0.20f;
		const float AdrPxAvail = Width * (1.0f - SustainPxFraction);
		const float SustainPx = Width * SustainPxFraction;

		float AttackPx;
		float DecayPx;
		float ReleasePx;
		if (TimeTotal <= 0.0f)
		{
			AttackPx = AdrPxAvail / 3.0f;
			DecayPx = AdrPxAvail / 3.0f;
			ReleasePx = AdrPxAvail / 3.0f;
		}
		else
		{
			AttackPx = AdrPxAvail * (AttackMs / TimeTotal);
			DecayPx = AdrPxAvail * (DecayMs / TimeTotal);
			ReleasePx = AdrPxAvail * (ReleaseMs / TimeTotal);
		}
		// Minimum visible width per stage.
		AttackPx = std::max(AttackPx, 6.0f);
		DecayPx = std::max(DecayPx, 6.0f);
		ReleasePx = std::max(ReleasePx, 6.0f);

		// -- Sample the curve at fixed pixel resolution -------------------------
		auto LevelToY = [&](float Level) -> float
		{
			return Max.y - Level * (Max.y - Min.y);
		};

		constexpr int32_t SamplesPerStage = 48;
		ImVec2 Pts[SamplesPerStage * 4 + 4];
		int32_t N = 0;

		float CursorX = Min.x;

		// Attack: 0 → 1
		for (int32_t I = 0; I <= SamplesPerStage; ++I)
		{
			const float T = static_cast<float>(I) / SamplesPerStage;
			Pts[N++] = ImVec2(CursorX + T * AttackPx, LevelToY(AttackCurve(T)));
		}
		CursorX += AttackPx;

		// Decay: 1 → Sustain
		for (int32_t I = 1; I <= SamplesPerStage; ++I)
		{
			const float T = static_cast<float>(I) / SamplesPerStage;
			Pts[N++] = ImVec2(CursorX + T * DecayPx, LevelToY(DecayCurve(T, Sustain)));
		}
		CursorX += DecayPx;

		// Sustain: flat
		Pts[N++] = ImVec2(CursorX + SustainPx, LevelToY(Sustain));
		CursorX += SustainPx;

		// Release: Sustain → 0
		for (int32_t I = 1; I <= SamplesPerStage; ++I)
		{
			const float T = static_cast<float>(I) / SamplesPerStage;
			Pts[N++] = ImVec2(CursorX + T * ReleasePx, LevelToY(ReleaseCurve(T, Sustain)));
		}

		// -- Filled area under the curve (subtle) -------------------------------
		const float Baseline = Max.y;
		for (int32_t I = 0; I < N - 1; ++I)
		{
			const ImVec2 Quad[4] =
			{
				ImVec2(Pts[I].x, Baseline),
				ImVec2(Pts[I].x, Pts[I].y),
				ImVec2(Pts[I + 1].x, Pts[I + 1].y),
				ImVec2(Pts[I + 1].x, Baseline),
			};
			Draw->AddConvexPolyFilled(Quad, 4, IM_COL32(80, 130, 200, 60));
		}

		// -- Curve outline ------------------------------------------------------
		Draw->AddPolyline(Pts, N, IM_COL32(180, 220, 255, 255), 0, 2.0f);

		// -- Stage boundary markers --------------------------------------------
		auto StageDivider = [&](float X)
		{
			Draw->AddLine(ImVec2(X, Min.y + 4.0f), ImVec2(X, Max.y - 4.0f),
				IM_COL32(100, 100, 120, 120), 1.0f);
		};
		float DivX = Min.x;
		DivX += AttackPx;
		StageDivider(DivX);
		DivX += DecayPx;
		StageDivider(DivX);
		DivX += SustainPx;
		StageDivider(DivX);

		// -- Stage labels along the top -----------------------------------------
		const ImU32 LabelCol = IM_COL32(140, 140, 160, 255);
		auto CenterLabel = [&](const char* Text, float CenterX)
		{
			const ImVec2 Size = ImGui::CalcTextSize(Text);
			Draw->AddText(ImVec2(CenterX - Size.x * 0.5f, Min.y + 2.0f), LabelCol, Text);
		};
		float Cx = Min.x;
		CenterLabel("A", Cx + AttackPx * 0.5f);
		Cx += AttackPx;
		CenterLabel("D", Cx + DecayPx * 0.5f);
		Cx += DecayPx;
		CenterLabel("S", Cx + SustainPx * 0.5f);
		Cx += SustainPx;
		CenterLabel("R", Cx + ReleasePx * 0.5f);
	}
}
