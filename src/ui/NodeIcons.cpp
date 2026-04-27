#include "ui/NodeIcons.h"

#include <cmath>
#include <cstring>

#include <imgui.h>

namespace NodeSynth
{
	namespace
	{
		// Per-category accent colours. Categories — sources (warm), control (green),
		// filters (blue), amplifiers (neutral), input (purple), sinks (red) — make the
		// graph readable at a glance.
		constexpr ImU32 ColSource = IM_COL32(255, 190, 100, 255);  // oscillators
		constexpr ImU32 ColControl = IM_COL32(140, 220, 130, 255); // ADSR, Gate
		constexpr ImU32 ColFilter = IM_COL32(120, 180, 255, 255);  // SVF
		constexpr ImU32 ColAmp = IM_COL32(220, 220, 220, 255);     // Gain, VCA
		constexpr ImU32 ColInput = IM_COL32(200, 150, 240, 255);   // MIDI, VirtualKbd
		constexpr ImU32 ColSink = IM_COL32(255, 130, 130, 255);    // Output
		constexpr ImU32 ColMath = IM_COL32(255, 220, 130, 255);    // Add/Mul/Scale/Const/S&H
		constexpr ImU32 ColEffect = IM_COL32(180, 160, 230, 255);  // Delay / Reverb / Waveshaper

		void DrawSineIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			constexpr int32_t Segments = 24;
			ImVec2 Pts[Segments + 1];
			const float Mid = (Min.y + Max.y) * 0.5f;
			const float Amp = (Max.y - Min.y) * 0.30f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				const float X = Min.x + T * (Max.x - Min.x);
				const float Y = Mid - Amp * std::sin(T * 2.0f * 3.14159265f);
				Pts[I] = ImVec2(X, Y);
			}
			Draw->AddPolyline(Pts, Segments + 1, Col, 0, 1.5f);
		}

		void DrawTriangleIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col, bool bFilled)
		{
			const float Inset = (Max.y - Min.y) * 0.15f;
			const ImVec2 A(Min.x + Inset, Min.y + Inset);
			const ImVec2 B(Min.x + Inset, Max.y - Inset);
			const ImVec2 C(Max.x - Inset, (Min.y + Max.y) * 0.5f);
			if (bFilled)
			{
				Draw->AddTriangleFilled(A, B, C, Col);
			}
			else
			{
				Draw->AddTriangle(A, B, C, Col, 1.5f);
			}
		}

		void DrawVcaIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			DrawTriangleIcon(Draw, Min, Max, Col, false);
			// Control input arrow from below into the bottom of the triangle.
			const float CenterX = (Min.x + Max.x) * 0.5f;
			const float Inset = (Max.y - Min.y) * 0.15f;
			Draw->AddLine(ImVec2(CenterX, Max.y), ImVec2(CenterX, Max.y - Inset - 2.0f), Col, 1.5f);
		}

		void DrawFilterIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Low-pass response: flat then rolling off.
			constexpr int32_t Segments = 24;
			ImVec2 Pts[Segments + 1];
			const float Mid = (Min.y + Max.y) * 0.5f;
			const float Amp = (Max.y - Min.y) * 0.35f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				const float X = Min.x + T * (Max.x - Min.x);
				// 1 / (1 + (T*4)^4) - sharp shoulder rolloff.
				const float Norm = T * 3.0f;
				const float Resp = 1.0f / (1.0f + Norm * Norm * Norm * Norm);
				const float Y = Mid - Amp * (Resp * 2.0f - 1.0f);
				Pts[I] = ImVec2(X, Y);
			}
			Draw->AddPolyline(Pts, Segments + 1, Col, 0, 1.5f);
		}

		void DrawAdsrIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Stylised ADSR curve: rise, fall to ~0.55, hold, fall to 0.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float Top = Min.y + H * 0.15f;
			const float Sustain = Min.y + H * 0.50f;
			const float Bottom = Max.y - H * 0.15f;
			const float X0 = Min.x;
			const float X1 = Min.x + W * 0.20f;  // peak
			const float X2 = Min.x + W * 0.45f;  // sustain start
			const float X3 = Min.x + W * 0.65f;  // release start
			const float X4 = Max.x;
			const ImVec2 Pts[5] =
			{
				ImVec2(X0, Bottom),
				ImVec2(X1, Top),
				ImVec2(X2, Sustain),
				ImVec2(X3, Sustain),
				ImVec2(X4, Bottom),
			};
			Draw->AddPolyline(Pts, 5, Col, 0, 1.5f);
		}

		void DrawGateIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Square pulse: low-high-low.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float Top = Min.y + H * 0.20f;
			const float Bottom = Max.y - H * 0.20f;
			const float X0 = Min.x;
			const float X1 = Min.x + W * 0.30f;
			const float X2 = Min.x + W * 0.70f;
			const float X3 = Max.x;
			const ImVec2 Pts[6] =
			{
				ImVec2(X0, Bottom),
				ImVec2(X1, Bottom),
				ImVec2(X1, Top),
				ImVec2(X2, Top),
				ImVec2(X2, Bottom),
				ImVec2(X3, Bottom),
			};
			Draw->AddPolyline(Pts, 6, Col, 0, 1.5f);
		}

		void DrawPianoIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 WhiteCol)
		{
			// 5 white keys + 3 black keys.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			constexpr int32_t NumWhites = 5;
			const float WhiteW = W / NumWhites;
			const float WhiteTop = Min.y + H * 0.10f;
			const float WhiteBot = Max.y - H * 0.10f;
			for (int32_t I = 0; I < NumWhites; ++I)
			{
				const float X0 = Min.x + I * WhiteW;
				const float X1 = X0 + WhiteW;
				Draw->AddRect(ImVec2(X0, WhiteTop), ImVec2(X1, WhiteBot), WhiteCol, 0.0f, 0, 1.0f);
			}
			// Black keys at gaps 0|1, 1|2, 3|4 (skipping 2|3 like a real keyboard).
			const float BlackW = WhiteW * 0.55f;
			const float BlackBot = WhiteTop + (WhiteBot - WhiteTop) * 0.65f;
			const int32_t BlackAfter[3] = { 0, 1, 3 };
			for (int32_t I = 0; I < 3; ++I)
			{
				const float CenterX = Min.x + (BlackAfter[I] + 1) * WhiteW;
				const float X0 = CenterX - BlackW * 0.5f;
				const float X1 = X0 + BlackW;
				Draw->AddRectFilled(ImVec2(X0, WhiteTop), ImVec2(X1, BlackBot), WhiteCol);
			}
		}

		void DrawSpeakerIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Trapezoid speaker body + two arc lines on the right.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float CenterY = (Min.y + Max.y) * 0.5f;
			const ImVec2 Body[4] =
			{
				ImVec2(Min.x + W * 0.05f, CenterY - H * 0.18f),
				ImVec2(Min.x + W * 0.30f, CenterY - H * 0.18f),
				ImVec2(Min.x + W * 0.50f, CenterY - H * 0.38f),
				ImVec2(Min.x + W * 0.50f, CenterY + H * 0.38f),
			};
			const ImVec2 Body2[4] =
			{
				ImVec2(Min.x + W * 0.50f, CenterY + H * 0.38f),
				ImVec2(Min.x + W * 0.30f, CenterY + H * 0.18f),
				ImVec2(Min.x + W * 0.05f, CenterY + H * 0.18f),
				ImVec2(Min.x + W * 0.05f, CenterY - H * 0.18f),
			};
			Draw->AddPolyline(Body, 4, Col, 0, 1.5f);
			Draw->AddPolyline(Body2, 4, Col, ImDrawFlags_Closed, 1.5f);

			// Sound waves on the right.
			const float ArcCx = Min.x + W * 0.55f;
			const float ArcCy = CenterY;
			const float R1 = H * 0.20f;
			const float R2 = H * 0.34f;
			Draw->PathArcTo(ImVec2(ArcCx, ArcCy), R1, -0.7f, 0.7f, 12);
			Draw->PathStroke(Col, 0, 1.5f);
			Draw->PathArcTo(ImVec2(ArcCx, ArcCy), R2, -0.7f, 0.7f, 16);
			Draw->PathStroke(Col, 0, 1.5f);
		}

		void DrawLfoIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Slow sine — one cycle, lower amplitude than the audio Oscillator's
			// 1.5-cycle icon to read as "modulation".
			constexpr int32_t Segments = 24;
			ImVec2 Pts[Segments + 1];
			const float Mid = (Min.y + Max.y) * 0.5f;
			const float Amp = (Max.y - Min.y) * 0.30f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				const float X = Min.x + T * (Max.x - Min.x);
				const float Y = Mid - Amp * std::sin(T * 2.0f * 3.14159265f);
				Pts[I] = ImVec2(X, Y);
			}
			Draw->AddPolyline(Pts, Segments + 1, Col, 0, 1.5f);
		}

		void DrawAddIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			const float Inset = (Max.y - Min.y) * 0.20f;
			const float Cx = (Min.x + Max.x) * 0.5f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			Draw->AddLine(ImVec2(Min.x + Inset, Cy), ImVec2(Max.x - Inset, Cy), Col, 1.8f);
			Draw->AddLine(ImVec2(Cx, Min.y + Inset), ImVec2(Cx, Max.y - Inset), Col, 1.8f);
		}

		void DrawMultiplyIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			const float Inset = (Max.y - Min.y) * 0.22f;
			Draw->AddLine(ImVec2(Min.x + Inset, Min.y + Inset),
				ImVec2(Max.x - Inset, Max.y - Inset), Col, 1.8f);
			Draw->AddLine(ImVec2(Max.x - Inset, Min.y + Inset),
				ImVec2(Min.x + Inset, Max.y - Inset), Col, 1.8f);
		}

		void DrawScaleIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Two diagonal lines with different slopes — the visual metaphor for
			// "remap a range to a different range."
			const float Inset = (Max.y - Min.y) * 0.18f;
			const float L = Min.x + Inset;
			const float R = Max.x - Inset;
			const float T = Min.y + Inset;
			const float B = Max.y - Inset;
			const float Mid = (T + B) * 0.5f;
			Draw->AddLine(ImVec2(L, B), ImVec2(R, T), Col, 1.5f);
			Draw->AddLine(ImVec2(L, B), ImVec2(R, Mid), Col, 1.5f);
		}

		void DrawConstantIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Horizontal line at the centre — DC value.
			const float Inset = (Max.y - Min.y) * 0.18f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			Draw->AddLine(ImVec2(Min.x + Inset, Cy), ImVec2(Max.x - Inset, Cy), Col, 2.0f);
		}

		void DrawSampleHoldIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Stair-step with three risers.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.15f;
			const float R = Max.x - W * 0.10f;
			const float T = Min.y + H * 0.20f;
			const float B = Max.y - H * 0.20f;
			const float StepW = (R - L) / 3.0f;
			const float StepH = (B - T) / 3.0f;
			const ImVec2 Pts[7] =
			{
				ImVec2(L,                B),
				ImVec2(L,                B - StepH),
				ImVec2(L + StepW,        B - StepH),
				ImVec2(L + StepW,        B - 2.0f * StepH),
				ImVec2(L + 2.0f * StepW, B - 2.0f * StepH),
				ImVec2(L + 2.0f * StepW, T),
				ImVec2(R,                T),
			};
			Draw->AddPolyline(Pts, 7, Col, 0, 1.5f);
		}

		void DrawWaveshaperIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Sigmoid-like S-curve: rises linearly through the middle, flattens
			// near ±1 — the visual signature of a soft saturator.
			constexpr int32_t Segments = 24;
			ImVec2 Pts[Segments + 1];
			const float Mid = (Min.y + Max.y) * 0.5f;
			const float Amp = (Max.y - Min.y) * 0.32f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;       // 0..1
				const float X = Min.x + T * (Max.x - Min.x);
				const float Z = (T - 0.5f) * 6.0f;                      // -3..3
				const float Y = Mid - Amp * std::tanh(Z);
				Pts[I] = ImVec2(X, Y);
			}
			Draw->AddPolyline(Pts, Segments + 1, Col, 0, 1.5f);
		}

		void DrawReverbIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Three concentric arcs suggesting expanding ripples.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float Cx = Min.x + W * 0.30f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float R0 = H * 0.18f;
			const float R1 = H * 0.30f;
			const float R2 = H * 0.42f;
			Draw->PathArcTo(ImVec2(Cx, Cy), R0, -0.7f, 0.7f, 14);
			Draw->PathStroke(Col, 0, 1.5f);
			Draw->PathArcTo(ImVec2(Cx, Cy), R1, -0.7f, 0.7f, 16);
			Draw->PathStroke(Col, 0, 1.5f);
			Draw->PathArcTo(ImVec2(Cx, Cy), R2, -0.7f, 0.7f, 18);
			Draw->PathStroke(Col, 0, 1.5f);
		}

		void DrawDelayIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Three echo "ticks" with diminishing height + a small return arrow
			// underneath suggesting the feedback path.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.10f;
			const float Bottom = Max.y - H * 0.30f;
			const float Top1 = Min.y + H * 0.20f;  // tallest
			const float Top2 = Min.y + H * 0.35f;
			const float Top3 = Min.y + H * 0.50f;

			const float X1 = L + W * 0.10f;
			const float X2 = L + W * 0.40f;
			const float X3 = L + W * 0.70f;

			Draw->AddLine(ImVec2(X1, Bottom), ImVec2(X1, Top1), Col, 1.5f);
			Draw->AddLine(ImVec2(X2, Bottom), ImVec2(X2, Top2), Col, 1.5f);
			Draw->AddLine(ImVec2(X3, Bottom), ImVec2(X3, Top3), Col, 1.5f);

			// Feedback arrow: arc from the rightmost tick back toward the left,
			// approximated as two line segments.
			const float ArcY = Bottom + H * 0.10f;
			Draw->AddLine(ImVec2(X3, Bottom), ImVec2(X3, ArcY), Col, 1.2f);
			Draw->AddLine(ImVec2(X3, ArcY), ImVec2(X1, ArcY), Col, 1.2f);
			Draw->AddLine(ImVec2(X1, ArcY), ImVec2(X1 + 4.0f, ArcY - 3.0f), Col, 1.2f);
			Draw->AddLine(ImVec2(X1, ArcY), ImVec2(X1 + 4.0f, ArcY + 3.0f), Col, 1.2f);
		}

		void DrawVoiceAllocatorIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Fan-out: one input on the left branching to four parallel outputs.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.18f;
			const float R = Max.x - W * 0.10f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float TopY = Min.y + H * 0.20f;
			const float BotY = Max.y - H * 0.20f;
			const float Mid1 = Cy - (Cy - TopY) * 0.45f;
			const float Mid2 = Cy + (BotY - Cy) * 0.45f;

			// Stem
			Draw->AddLine(ImVec2(Min.x + W * 0.05f, Cy), ImVec2(L, Cy), Col, 1.5f);
			// Vertical riser
			Draw->AddLine(ImVec2(L, TopY), ImVec2(L, BotY), Col, 1.5f);
			// Four horizontal branches to the right edge
			Draw->AddLine(ImVec2(L, TopY), ImVec2(R, TopY), Col, 1.5f);
			Draw->AddLine(ImVec2(L, Mid1), ImVec2(R, Mid1), Col, 1.5f);
			Draw->AddLine(ImVec2(L, Mid2), ImVec2(R, Mid2), Col, 1.5f);
			Draw->AddLine(ImVec2(L, BotY), ImVec2(R, BotY), Col, 1.5f);
		}

		void DrawDefaultIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Generic node: a small square outline.
			const float Inset = (Max.y - Min.y) * 0.20f;
			Draw->AddRect(ImVec2(Min.x + Inset, Min.y + Inset),
				ImVec2(Max.x - Inset, Max.y - Inset), Col, 1.0f, 0, 1.0f);
		}
	}

	void DrawNodeIcon(const char* TypeName, ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max)
	{
		if (TypeName == nullptr || Draw == nullptr)
		{
			return;
		}

		if (std::strcmp(TypeName, "Oscillator") == 0)
		{
			DrawSineIcon(Draw, Min, Max, ColSource);
		}
		else if (std::strcmp(TypeName, "Gain") == 0)
		{
			DrawTriangleIcon(Draw, Min, Max, ColAmp, true);
		}
		else if (std::strcmp(TypeName, "VCA") == 0)
		{
			DrawVcaIcon(Draw, Min, Max, ColAmp);
		}
		else if (std::strcmp(TypeName, "SVF") == 0)
		{
			DrawFilterIcon(Draw, Min, Max, ColFilter);
		}
		else if (std::strcmp(TypeName, "ADSR") == 0)
		{
			DrawAdsrIcon(Draw, Min, Max, ColControl);
		}
		else if (std::strcmp(TypeName, "Gate") == 0)
		{
			DrawGateIcon(Draw, Min, Max, ColControl);
		}
		else if (std::strcmp(TypeName, "MIDI") == 0)
		{
			DrawPianoIcon(Draw, Min, Max, ColInput);
		}
		else if (std::strcmp(TypeName, "VirtualKbd") == 0)
		{
			DrawPianoIcon(Draw, Min, Max, ColInput);
		}
		else if (std::strcmp(TypeName, "Output") == 0)
		{
			DrawSpeakerIcon(Draw, Min, Max, ColSink);
		}
		else if (std::strcmp(TypeName, "Add") == 0)
		{
			DrawAddIcon(Draw, Min, Max, ColMath);
		}
		else if (std::strcmp(TypeName, "Multiply") == 0)
		{
			DrawMultiplyIcon(Draw, Min, Max, ColMath);
		}
		else if (std::strcmp(TypeName, "Scale") == 0)
		{
			DrawScaleIcon(Draw, Min, Max, ColMath);
		}
		else if (std::strcmp(TypeName, "Constant") == 0)
		{
			DrawConstantIcon(Draw, Min, Max, ColMath);
		}
		else if (std::strcmp(TypeName, "SampleHold") == 0)
		{
			DrawSampleHoldIcon(Draw, Min, Max, ColMath);
		}
		else if (std::strcmp(TypeName, "LFO") == 0)
		{
			DrawLfoIcon(Draw, Min, Max, ColMath);
		}
		else if (std::strcmp(TypeName, "VoiceAllocator") == 0)
		{
			DrawVoiceAllocatorIcon(Draw, Min, Max, ColInput);
		}
		else if (std::strcmp(TypeName, "Delay") == 0)
		{
			DrawDelayIcon(Draw, Min, Max, ColEffect);
		}
		else if (std::strcmp(TypeName, "Reverb") == 0)
		{
			DrawReverbIcon(Draw, Min, Max, ColEffect);
		}
		else if (std::strcmp(TypeName, "Waveshaper") == 0)
		{
			DrawWaveshaperIcon(Draw, Min, Max, ColEffect);
		}
		else
		{
			DrawDefaultIcon(Draw, Min, Max, ColAmp);
		}
	}

	void IconBeforeText(const char* TypeName, float Size)
	{
		const ImVec2 Cursor = ImGui::GetCursorScreenPos();
		const ImVec2 Min(Cursor.x, Cursor.y);
		const ImVec2 Max(Cursor.x + Size, Cursor.y + Size);
		DrawNodeIcon(TypeName, ImGui::GetWindowDrawList(), Min, Max);
		ImGui::Dummy(ImVec2(Size, Size));
		ImGui::SameLine(0.0f, 6.0f);
	}
}
