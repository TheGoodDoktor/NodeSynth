#include "ui/NodeIcons.h"

#include <algorithm>
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

		void DrawScopeIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Outline frame + sine trace inside (oscilloscope visual).
			const float Inset = (Max.y - Min.y) * 0.18f;
			Draw->AddRect(
				ImVec2(Min.x + Inset, Min.y + Inset),
				ImVec2(Max.x - Inset, Max.y - Inset),
				Col, 1.0f, 0, 1.0f);

			constexpr int32_t Segments = 24;
			ImVec2 Pts[Segments + 1];
			const float L = Min.x + Inset + 2.0f;
			const float R = Max.x - Inset - 2.0f;
			const float Mid = (Min.y + Max.y) * 0.5f;
			const float Amp = (Max.y - Min.y) * 0.18f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				const float X = L + T * (R - L);
				const float Y = Mid - Amp * std::sin(T * 2.0f * 3.14159265f);
				Pts[I] = ImVec2(X, Y);
			}
			Draw->AddPolyline(Pts, Segments + 1, Col, 0, 1.5f);
		}

		void DrawMeterIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Vertical bar with three tick marks on the right (VU-style).
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.30f;
			const float R = Max.x - W * 0.20f;
			const float T = Min.y + H * 0.18f;
			const float B = Max.y - H * 0.18f;
			Draw->AddRect(ImVec2(L, T), ImVec2(R, B), Col, 1.0f, 0, 1.0f);
			// Fill the lower half to suggest a level reading.
			const float MidY = (T + B) * 0.5f;
			Draw->AddRectFilled(ImVec2(L + 1.0f, MidY), ImVec2(R - 1.0f, B - 1.0f), Col);
			// Tick marks on the left.
			const float TickX1 = L - W * 0.08f;
			const float TickX2 = L - 1.0f;
			for (int32_t I = 0; I < 4; ++I)
			{
				const float Y = T + (B - T) * (static_cast<float>(I) / 3.0f);
				Draw->AddLine(ImVec2(TickX1, Y), ImVec2(TickX2, Y), Col, 1.0f);
			}
		}

		void DrawClockIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Circle with two hands at 12 and 3 o'clock.
			const float Cx = (Min.x + Max.x) * 0.5f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float R = std::min(Max.x - Min.x, Max.y - Min.y) * 0.35f;
			Draw->AddCircle(ImVec2(Cx, Cy), R, Col, 16, 1.5f);
			Draw->AddLine(ImVec2(Cx, Cy), ImVec2(Cx, Cy - R * 0.65f), Col, 1.5f);
			Draw->AddLine(ImVec2(Cx, Cy), ImVec2(Cx + R * 0.5f, Cy), Col, 1.5f);
		}

		void DrawSequencerIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Eight stylised step bars at varying heights.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.10f;
			const float R = Max.x - W * 0.10f;
			const float Bottom = Max.y - H * 0.20f;
			constexpr int32_t NumBars = 8;
			constexpr float Heights[NumBars] = {
				0.50f, 0.30f, 0.65f, 0.40f, 0.55f, 0.25f, 0.70f, 0.45f
			};
			const float StepW = (R - L) / NumBars;
			for (int32_t I = 0; I < NumBars; ++I)
			{
				const float X = L + (static_cast<float>(I) + 0.5f) * StepW;
				const float Top = Bottom - Heights[I] * H * 0.7f;
				Draw->AddLine(ImVec2(X, Bottom), ImVec2(X, Top), Col, 1.5f);
			}
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

		void DrawMixerIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Fan-in: four parallel inputs on the left merging into one output
			// on the right. Mirror of the voice-allocator's fan-out icon.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.10f;
			const float R = Max.x - W * 0.10f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float TopY = Min.y + H * 0.20f;
			const float BotY = Max.y - H * 0.20f;
			const float Mid1 = Cy - (Cy - TopY) * 0.45f;
			const float Mid2 = Cy + (BotY - Cy) * 0.45f;
			const float JoinX = Min.x + W * 0.65f;

			// Four input branches (left).
			Draw->AddLine(ImVec2(L, TopY), ImVec2(JoinX, TopY), Col, 1.5f);
			Draw->AddLine(ImVec2(L, Mid1), ImVec2(JoinX, Mid1), Col, 1.5f);
			Draw->AddLine(ImVec2(L, Mid2), ImVec2(JoinX, Mid2), Col, 1.5f);
			Draw->AddLine(ImVec2(L, BotY), ImVec2(JoinX, BotY), Col, 1.5f);
			// Vertical join.
			Draw->AddLine(ImVec2(JoinX, TopY), ImVec2(JoinX, BotY), Col, 1.5f);
			// Output stem.
			Draw->AddLine(ImVec2(JoinX, Cy), ImVec2(R, Cy), Col, 1.5f);
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

		// Dim variant of an accent colour for secondary glyph elements
		// (reference lines, slider tracks, etc.). 50% alpha.
		ImU32 DimColor(ImU32 Col)
		{
			return (Col & 0x00FFFFFFu) | 0x80000000u;
		}

		void DrawChorusIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Three stacked, slightly phase-shifted sine waves — voicing stack.
			constexpr int32_t Segments = 20;
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float Amp = H * 0.13f;
			const float Spread = H * 0.18f;
			const float YOff[3] = { -Spread, 0.0f, Spread };
			const float PhaseOff[3] = { 0.0f, 0.6f, 1.2f };
			for (int32_t L = 0; L < 3; ++L)
			{
				ImVec2 Pts[Segments + 1];
				for (int32_t I = 0; I <= Segments; ++I)
				{
					const float T = static_cast<float>(I) / Segments;
					const float X = Min.x + W * 0.10f + T * W * 0.80f;
					const float Y = Cy + YOff[L] - Amp * std::sin(T * 2.0f * 3.14159265f + PhaseOff[L]);
					Pts[I] = ImVec2(X, Y);
				}
				Draw->AddPolyline(Pts, Segments + 1, Col, 0, 1.2f);
			}
		}

		void DrawFlangerIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Comb-filter response: cosine-squared peaks/notches across the band.
			constexpr int32_t Segments = 32;
			ImVec2 Pts[Segments + 1];
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.10f;
			const float R = Max.x - W * 0.10f;
			const float Mid = (Min.y + Max.y) * 0.5f;
			const float Amp = H * 0.30f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				const float X = L + T * (R - L);
				const float Y = Mid - Amp * std::cos(T * 4.0f * 3.14159265f);
				Pts[I] = ImVec2(X, Y);
			}
			Draw->AddPolyline(Pts, Segments + 1, Col, 0, 1.5f);
		}

		void DrawPhaserIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Spectrum with a single deep notch in the centre — phaser signature.
			constexpr int32_t Segments = 32;
			ImVec2 Pts[Segments + 1];
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.10f;
			const float R = Max.x - W * 0.10f;
			const float Top = Min.y + H * 0.25f;
			const float Bot = Max.y - H * 0.20f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				const float X = L + T * (R - L);
				const float D = (T - 0.5f) * 5.0f;       // -2.5 .. +2.5
				const float Notch = std::exp(-D * D);    // Gaussian dip
				const float Y = Top + (Bot - Top) * Notch;
				Pts[I] = ImVec2(X, Y);
			}
			Draw->AddPolyline(Pts, Segments + 1, Col, 0, 1.5f);
		}

		void DrawCompressorIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Compression transfer curve: 1:1 below threshold, then shallower.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.18f;
			const float R = Max.x - W * 0.10f;
			const float B = Max.y - H * 0.18f;
			const float T = Min.y + H * 0.18f;
			const float KneeX = L + (R - L) * 0.45f;
			const float KneeY = B - (B - T) * 0.55f;
			Draw->AddLine(ImVec2(L, B), ImVec2(KneeX, KneeY), Col, 1.5f);
			Draw->AddLine(ImVec2(KneeX, KneeY), ImVec2(R, T + (B - T) * 0.18f), Col, 1.5f);
			Draw->AddLine(ImVec2(KneeX, B), ImVec2(KneeX, KneeY), DimColor(Col), 1.0f);
		}

		void DrawLimiterIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Brickwall: ramp up then completely flat ceiling.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.18f;
			const float R = Max.x - W * 0.10f;
			const float B = Max.y - H * 0.18f;
			const float Ceiling = Min.y + H * 0.30f;
			const float HingeX = L + (R - L) * 0.45f;
			Draw->AddLine(ImVec2(L, B), ImVec2(HingeX, Ceiling), Col, 1.5f);
			Draw->AddLine(ImVec2(HingeX, Ceiling), ImVec2(R, Ceiling), Col, 1.5f);
			Draw->AddLine(ImVec2(L, Ceiling - 4.0f), ImVec2(R, Ceiling - 4.0f), DimColor(Col), 1.0f);
		}

		void DrawNoiseGateIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Step transfer: zero floor, then opens to constant level past threshold.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.15f;
			const float R = Max.x - W * 0.10f;
			const float B = Max.y - H * 0.22f;
			const float T = Min.y + H * 0.22f;
			const float StepX = L + (R - L) * 0.45f;
			Draw->AddLine(ImVec2(L, B), ImVec2(StepX, B), Col, 1.5f);
			Draw->AddLine(ImVec2(StepX, B), ImVec2(StepX, T), Col, 1.5f);
			Draw->AddLine(ImVec2(StepX, T), ImVec2(R, T), Col, 1.5f);
		}

		void DrawEqualizerIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Three vertical EQ slider tracks with knobs at different heights.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float L = Min.x + W * 0.22f;
			const float R = Max.x - W * 0.22f;
			const float TrackHalfH = H * 0.30f;
			constexpr int32_t NumBands = 3;
			const float Spacing = (R - L) / (NumBands - 1);
			const float Heights[NumBands] = { -0.45f, 0.55f, -0.20f };
			const ImU32 TrackCol = DimColor(Col);
			for (int32_t I = 0; I < NumBands; ++I)
			{
				const float X = L + I * Spacing;
				Draw->AddLine(ImVec2(X, Cy - TrackHalfH), ImVec2(X, Cy + TrackHalfH), TrackCol, 1.0f);
				const float KnobY = Cy + Heights[I] * TrackHalfH;
				Draw->AddRectFilled(ImVec2(X - 3.0f, KnobY - 2.0f),
					ImVec2(X + 3.0f, KnobY + 2.0f), Col);
			}
		}

		void DrawDcBlockerIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Sine sitting above the centre line, with the centre line drawn
			// dimly to suggest "remove the offset, return to zero".
			constexpr int32_t Segments = 24;
			ImVec2 Pts[Segments + 1];
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.10f;
			const float R = Max.x - W * 0.10f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float OffsetY = -H * 0.20f;
			const float Amp = H * 0.18f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				const float X = L + T * (R - L);
				const float Y = Cy + OffsetY - Amp * std::sin(T * 2.0f * 3.14159265f);
				Pts[I] = ImVec2(X, Y);
			}
			Draw->AddPolyline(Pts, Segments + 1, Col, 0, 1.5f);
			Draw->AddLine(ImVec2(L, Cy), ImVec2(R, Cy), DimColor(Col), 1.0f);
		}

		void DrawTremoloIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// High-freq sine modulated by a slow envelope (classic AM shape).
			constexpr int32_t Segments = 48;
			ImVec2 Pts[Segments + 1];
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.10f;
			const float R = Max.x - W * 0.10f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float MaxAmp = H * 0.32f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				const float X = L + T * (R - L);
				const float Env = 0.5f - 0.5f * std::cos(T * 2.0f * 3.14159265f);
				const float Y = Cy - MaxAmp * Env * std::sin(T * 14.0f * 3.14159265f);
				Pts[I] = ImVec2(X, Y);
			}
			Draw->AddPolyline(Pts, Segments + 1, Col, 0, 1.4f);
		}

		void DrawAutoPanIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Sine-shaped path between left and right "speakers" — the pan sweep.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.18f;
			const float R = Max.x - W * 0.18f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			constexpr int32_t Segments = 24;
			ImVec2 Pts[Segments + 1];
			const float Amp = H * 0.22f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				const float X = L + T * (R - L);
				const float Y = Cy - Amp * std::sin(T * 2.0f * 3.14159265f);
				Pts[I] = ImVec2(X, Y);
			}
			Draw->AddPolyline(Pts, Segments + 1, Col, 0, 1.4f);
			Draw->AddCircleFilled(ImVec2(L, Cy), 2.5f, Col, 8);
			Draw->AddCircleFilled(ImVec2(R, Cy), 2.5f, Col, 8);
		}

		void DrawBitcrusherIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Stair-stepped sine — sample-rate + bit-depth quantization signature.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.10f;
			const float R = Max.x - W * 0.10f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float Amp = H * 0.28f;
			constexpr int32_t Steps = 12;
			constexpr float Levels = 4.0f;
			constexpr int32_t MaxPts = Steps * 2 + 2;
			ImVec2 Pts[MaxPts];
			int32_t N = 0;
			float PrevY = Cy;
			for (int32_t I = 0; I < Steps; ++I)
			{
				const float T0 = static_cast<float>(I) / Steps;
				const float T1 = static_cast<float>(I + 1) / Steps;
				const float X0 = L + T0 * (R - L);
				const float X1 = L + T1 * (R - L);
				const float Raw = std::sin((T0 + T1) * 0.5f * 2.0f * 3.14159265f);
				const float Q = std::floor(Raw * Levels) / Levels;
				const float Y = Cy - Amp * Q;
				if (I > 0 && N < MaxPts) { Pts[N++] = ImVec2(X0, PrevY); }
				if (N < MaxPts)          { Pts[N++] = ImVec2(X0, Y); }
				if (N < MaxPts)          { Pts[N++] = ImVec2(X1, Y); }
				PrevY = Y;
			}
			Draw->AddPolyline(Pts, N, Col, 0, 1.4f);
		}

		void DrawRingModIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Ring (circle) with an × inside — the multiplication metaphor.
			const float Cx = (Min.x + Max.x) * 0.5f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float Rad = std::min(Max.x - Min.x, Max.y - Min.y) * 0.32f;
			Draw->AddCircle(ImVec2(Cx, Cy), Rad, Col, 18, 1.5f);
			const float O = Rad * 0.55f;
			Draw->AddLine(ImVec2(Cx - O, Cy - O), ImVec2(Cx + O, Cy + O), Col, 1.5f);
			Draw->AddLine(ImVec2(Cx + O, Cy - O), ImVec2(Cx - O, Cy + O), Col, 1.5f);
		}

		void DrawStereoWidenerIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Outward-pointing arrows from a centre line — the "widen" gesture.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float Cx = (Min.x + Max.x) * 0.5f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float Inset = W * 0.16f;
			const float ArrHead = H * 0.10f;
			Draw->AddLine(ImVec2(Cx, Min.y + H * 0.20f),
				ImVec2(Cx, Max.y - H * 0.20f), DimColor(Col), 1.0f);
			const float Lx = Min.x + Inset;
			Draw->AddLine(ImVec2(Cx, Cy), ImVec2(Lx, Cy), Col, 1.5f);
			Draw->AddLine(ImVec2(Lx, Cy), ImVec2(Lx + ArrHead, Cy - ArrHead), Col, 1.5f);
			Draw->AddLine(ImVec2(Lx, Cy), ImVec2(Lx + ArrHead, Cy + ArrHead), Col, 1.5f);
			const float Rx = Max.x - Inset;
			Draw->AddLine(ImVec2(Cx, Cy), ImVec2(Rx, Cy), Col, 1.5f);
			Draw->AddLine(ImVec2(Rx, Cy), ImVec2(Rx - ArrHead, Cy - ArrHead), Col, 1.5f);
			Draw->AddLine(ImVec2(Rx, Cy), ImVec2(Rx - ArrHead, Cy + ArrHead), Col, 1.5f);
		}

		void DrawHaasWidenerIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Two stacked sine pulses; the lower one starts later — Haas delay.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.08f;
			const float R = Max.x - W * 0.05f;
			const float TopY = Min.y + H * 0.30f;
			const float BotY = Max.y - H * 0.30f;
			const float Amp = H * 0.10f;
			constexpr int32_t Segments = 16;
			ImVec2 Top[Segments + 1];
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				Top[I] = ImVec2(L + T * (R - L),
					TopY - Amp * std::sin(T * 2.0f * 3.14159265f));
			}
			Draw->AddPolyline(Top, Segments + 1, Col, 0, 1.3f);
			ImVec2 Bot[Segments + 1];
			const float Shift = W * 0.20f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				Bot[I] = ImVec2(L + Shift + T * (R - L - Shift),
					BotY - Amp * std::sin(T * 2.0f * 3.14159265f));
			}
			Draw->AddPolyline(Bot, Segments + 1, Col, 0, 1.3f);
		}

		void DrawExciterIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// 6-spoke sparkle above a sine "ground" — high-frequency air on
			// top of the audio signal.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float Cx = (Min.x + Max.x) * 0.5f;
			const float SparkY = Min.y + H * 0.32f;
			const float Rad = H * 0.20f;
			for (int32_t I = 0; I < 6; ++I)
			{
				const float A = (static_cast<float>(I) / 6.0f) * 2.0f * 3.14159265f;
				const float Sx = Cx + std::cos(A) * Rad;
				const float Sy = SparkY + std::sin(A) * Rad;
				Draw->AddLine(ImVec2(Cx, SparkY), ImVec2(Sx, Sy), Col, 1.4f);
			}
			Draw->AddCircleFilled(ImVec2(Cx, SparkY), 1.5f, Col);
			constexpr int32_t Segments = 20;
			ImVec2 Pts[Segments + 1];
			const float L = Min.x + W * 0.10f;
			const float R = Max.x - W * 0.10f;
			const float GroundY = Max.y - H * 0.22f;
			const float GroundAmp = H * 0.09f;
			for (int32_t I = 0; I <= Segments; ++I)
			{
				const float T = static_cast<float>(I) / Segments;
				Pts[I] = ImVec2(L + T * (R - L),
					GroundY - GroundAmp * std::sin(T * 2.0f * 3.14159265f));
			}
			Draw->AddPolyline(Pts, Segments + 1, DimColor(Col), 0, 1.2f);
		}

		void DrawModMatrixIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// 3x3 dot grid — one filled, others outlined — to read as
			// "matrix of routings" without ambiguity vs. other Modulation
			// icons (LFO sine, Sequencer steps, S&H stairs, Clock face).
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float Cx = (Min.x + Max.x) * 0.5f;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float Span = std::min(W, H) * 0.55f;
			const float DotR = std::min(W, H) * 0.06f;
			const float Step = Span * 0.5f;
			for (int32_t Row = 0; Row < 3; ++Row)
			{
				for (int32_t ColIdx = 0; ColIdx < 3; ++ColIdx)
				{
					const ImVec2 Centre(Cx + (ColIdx - 1) * Step, Cy + (Row - 1) * Step);
					// Filled dot at (1,2) gives the icon a deliberate
					// "one route active" hint without forcing the user
					// to read it that way.
					if (Row == 1 && ColIdx == 2)
					{
						Draw->AddCircleFilled(Centre, DotR, Col, 12);
					}
					else
					{
						Draw->AddCircle(Centre, DotR, Col, 12, 1.2f);
					}
				}
			}
		}

		void DrawMidiCCIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Horizontal slider: track line with a knob marker at ~1/3
			// position. Reads as "MIDI controller" without ambiguity.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float Cy = (Min.y + Max.y) * 0.5f;
			const float L = Min.x + W * 0.15f;
			const float R = Max.x - W * 0.15f;
			const ImU32 TrackCol = (Col & 0x00FFFFFFu) | 0x80000000u;
			Draw->AddLine(ImVec2(L, Cy), ImVec2(R, Cy), TrackCol, 1.5f);
			const float KnobX = L + (R - L) * 0.35f;
			Draw->AddCircleFilled(ImVec2(KnobX, Cy), H * 0.16f, Col, 12);
			// "CC" hint: two small ticks above the slider track.
			const float TickY = Cy - H * 0.30f;
			Draw->AddLine(ImVec2(L + W * 0.10f, TickY),
				ImVec2(L + W * 0.10f, TickY + H * 0.10f), Col, 1.4f);
			Draw->AddLine(ImVec2(L + W * 0.22f, TickY),
				ImVec2(L + W * 0.22f, TickY + H * 0.10f), Col, 1.4f);
		}

		void DrawWavetableIcon(ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max, ImU32 Col)
		{
			// Stack of three single-cycle waveforms with a perspective tilt:
			// front line is a saw, middle is a triangle, back is a sine.
			// Suggests the "frames stacked" mental model.
			const float W = Max.x - Min.x;
			const float H = Max.y - Min.y;
			const float L = Min.x + W * 0.10f;
			const float R = Max.x - W * 0.10f;
			const float Amp = H * 0.10f;
			constexpr int32_t Segments = 18;

			auto DrawWave = [&](float Cy, float Skew, int32_t Kind, float Alpha)
			{
				const ImU32 LineCol = (Col & 0x00FFFFFFu) | (static_cast<uint32_t>(Alpha * 255.0f) << 24);
				ImVec2 Pts[Segments + 1];
				for (int32_t I = 0; I <= Segments; ++I)
				{
					const float T = static_cast<float>(I) / Segments;
					const float X = L + Skew + T * (R - L);
					float Y = 0.0f;
					switch (Kind)
					{
						case 0: // sine
							Y = -Amp * std::sin(T * 2.0f * 3.14159265f);
							break;
						case 1: // triangle
							Y = -Amp * (1.0f - 2.0f * std::fabs(2.0f * std::fmod(T + 0.25f, 1.0f) - 1.0f));
							break;
						case 2: // saw
							Y = -Amp * (2.0f * T - 1.0f);
							break;
						default: break;
					}
					Pts[I] = ImVec2(X, Cy + Y);
				}
				Draw->AddPolyline(Pts, Segments + 1, LineCol, 0, 1.3f);
			};

			DrawWave(Min.y + H * 0.30f, +W * 0.04f, 0, 0.55f);  // back: sine, faded
			DrawWave(Min.y + H * 0.55f, +W * 0.02f, 1, 0.80f);  // mid:  triangle
			DrawWave(Min.y + H * 0.80f,  0.0f,      2, 1.00f);  // front: saw, full
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
		else if (std::strcmp(TypeName, "Mixer") == 0)
		{
			DrawMixerIcon(Draw, Min, Max, ColAmp);
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
		else if (std::strcmp(TypeName, "Clock") == 0)
		{
			DrawClockIcon(Draw, Min, Max, ColControl);
		}
		else if (std::strcmp(TypeName, "Sequencer") == 0)
		{
			DrawSequencerIcon(Draw, Min, Max, ColControl);
		}
		else if (std::strcmp(TypeName, "Scope") == 0)
		{
			DrawScopeIcon(Draw, Min, Max, ColAmp);
		}
		else if (std::strcmp(TypeName, "Meter") == 0)
		{
			DrawMeterIcon(Draw, Min, Max, ColAmp);
		}
		else if (std::strcmp(TypeName, "Chorus") == 0)
		{
			DrawChorusIcon(Draw, Min, Max, ColEffect);
		}
		else if (std::strcmp(TypeName, "Flanger") == 0)
		{
			DrawFlangerIcon(Draw, Min, Max, ColEffect);
		}
		else if (std::strcmp(TypeName, "Phaser") == 0)
		{
			DrawPhaserIcon(Draw, Min, Max, ColEffect);
		}
		else if (std::strcmp(TypeName, "Compressor") == 0)
		{
			DrawCompressorIcon(Draw, Min, Max, ColAmp);
		}
		else if (std::strcmp(TypeName, "Limiter") == 0)
		{
			DrawLimiterIcon(Draw, Min, Max, ColAmp);
		}
		else if (std::strcmp(TypeName, "NoiseGate") == 0)
		{
			DrawNoiseGateIcon(Draw, Min, Max, ColAmp);
		}
		else if (std::strcmp(TypeName, "Equalizer") == 0)
		{
			DrawEqualizerIcon(Draw, Min, Max, ColFilter);
		}
		else if (std::strcmp(TypeName, "DcBlocker") == 0)
		{
			DrawDcBlockerIcon(Draw, Min, Max, ColFilter);
		}
		else if (std::strcmp(TypeName, "Tremolo") == 0)
		{
			DrawTremoloIcon(Draw, Min, Max, ColEffect);
		}
		else if (std::strcmp(TypeName, "AutoPan") == 0)
		{
			DrawAutoPanIcon(Draw, Min, Max, ColEffect);
		}
		else if (std::strcmp(TypeName, "Bitcrusher") == 0)
		{
			DrawBitcrusherIcon(Draw, Min, Max, ColEffect);
		}
		else if (std::strcmp(TypeName, "RingMod") == 0)
		{
			DrawRingModIcon(Draw, Min, Max, ColEffect);
		}
		else if (std::strcmp(TypeName, "StereoWidener") == 0)
		{
			DrawStereoWidenerIcon(Draw, Min, Max, ColAmp);
		}
		else if (std::strcmp(TypeName, "HaasWidener") == 0)
		{
			DrawHaasWidenerIcon(Draw, Min, Max, ColAmp);
		}
		else if (std::strcmp(TypeName, "Exciter") == 0)
		{
			DrawExciterIcon(Draw, Min, Max, ColEffect);
		}
		else if (std::strcmp(TypeName, "WavetableOscillator") == 0)
		{
			DrawWavetableIcon(Draw, Min, Max, ColSource);
		}
		else if (std::strcmp(TypeName, "MidiCC") == 0)
		{
			DrawMidiCCIcon(Draw, Min, Max, ColInput);
		}
		else if (std::strcmp(TypeName, "ModulationMatrix") == 0)
		{
			DrawModMatrixIcon(Draw, Min, Max, ColMath);
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

	unsigned int GetCategoryColor(const char* TypeName)
	{
		// Same dispatch table as DrawNodeIcon, returning the accent colour
		// instead of drawing the glyph. Order matches DrawNodeIcon for
		// auditability — when adding new node types, update both.
		if (TypeName == nullptr) { return ColAmp; }
		if (std::strcmp(TypeName, "Oscillator") == 0)     { return ColSource; }
		if (std::strcmp(TypeName, "Gain") == 0)           { return ColAmp; }
		if (std::strcmp(TypeName, "VCA") == 0)            { return ColAmp; }
		if (std::strcmp(TypeName, "SVF") == 0)            { return ColFilter; }
		if (std::strcmp(TypeName, "ADSR") == 0)           { return ColControl; }
		if (std::strcmp(TypeName, "Gate") == 0)           { return ColControl; }
		if (std::strcmp(TypeName, "Output") == 0)         { return ColSink; }
		if (std::strcmp(TypeName, "Add") == 0)            { return ColMath; }
		if (std::strcmp(TypeName, "Multiply") == 0)       { return ColMath; }
		if (std::strcmp(TypeName, "Scale") == 0)          { return ColMath; }
		if (std::strcmp(TypeName, "Constant") == 0)       { return ColMath; }
		if (std::strcmp(TypeName, "SampleHold") == 0)     { return ColMath; }
		if (std::strcmp(TypeName, "LFO") == 0)            { return ColMath; }
		if (std::strcmp(TypeName, "VoiceAllocator") == 0) { return ColInput; }
		if (std::strcmp(TypeName, "Mixer") == 0)          { return ColAmp; }
		if (std::strcmp(TypeName, "Delay") == 0)          { return ColEffect; }
		if (std::strcmp(TypeName, "Reverb") == 0)         { return ColEffect; }
		if (std::strcmp(TypeName, "Waveshaper") == 0)     { return ColEffect; }
		if (std::strcmp(TypeName, "Chorus") == 0)         { return ColEffect; }
		if (std::strcmp(TypeName, "Flanger") == 0)        { return ColEffect; }
		if (std::strcmp(TypeName, "Phaser") == 0)         { return ColEffect; }
		if (std::strcmp(TypeName, "Compressor") == 0)     { return ColAmp; }
		if (std::strcmp(TypeName, "Limiter") == 0)        { return ColAmp; }
		if (std::strcmp(TypeName, "NoiseGate") == 0)      { return ColAmp; }
		if (std::strcmp(TypeName, "Equalizer") == 0)      { return ColFilter; }
		if (std::strcmp(TypeName, "DcBlocker") == 0)      { return ColFilter; }
		if (std::strcmp(TypeName, "Tremolo") == 0)        { return ColEffect; }
		if (std::strcmp(TypeName, "AutoPan") == 0)        { return ColEffect; }
		if (std::strcmp(TypeName, "Bitcrusher") == 0)     { return ColEffect; }
		if (std::strcmp(TypeName, "RingMod") == 0)        { return ColEffect; }
		if (std::strcmp(TypeName, "StereoWidener") == 0)  { return ColAmp; }
		if (std::strcmp(TypeName, "HaasWidener") == 0)    { return ColAmp; }
		if (std::strcmp(TypeName, "Exciter") == 0)        { return ColEffect; }
		if (std::strcmp(TypeName, "Clock") == 0)          { return ColControl; }
		if (std::strcmp(TypeName, "Sequencer") == 0)      { return ColControl; }
		if (std::strcmp(TypeName, "Scope") == 0)          { return ColAmp; }
		if (std::strcmp(TypeName, "Meter") == 0)          { return ColAmp; }
		if (std::strcmp(TypeName, "SidPlayer") == 0)      { return ColInput; }
		if (std::strcmp(TypeName, "WavetableOscillator") == 0) { return ColSource; }
		if (std::strcmp(TypeName, "MidiCC") == 0)         { return ColInput; }
		if (std::strcmp(TypeName, "ModulationMatrix") == 0) { return ColMath; }
		return ColAmp;
	}

	void DrawPinIcon(EPortType Type, bool bConnected, float Size)
	{
		// All pins draw as circles; colour distinguishes the port type.
		// Audio = warm orange, Control = teal. Filled when connected,
		// outlined when not.
		const ImU32 AudioCol   = IM_COL32(255, 170,  60, 255);
		const ImU32 ControlCol = IM_COL32( 80, 220, 200, 255);
		const ImU32 Col = (Type == EPortType::Audio) ? AudioCol : ControlCol;

		ImDrawList* Draw = ImGui::GetWindowDrawList();
		const ImVec2 Cursor = ImGui::GetCursorScreenPos();
		// Centre the icon vertically against a baseline of one text line so
		// it visually aligns with the port name to its right.
		const float LineH = ImGui::GetTextLineHeight();
		const float Pad = (LineH - Size) * 0.5f;
		const ImVec2 Centre(Cursor.x + Size * 0.5f, Cursor.y + Pad + Size * 0.5f);
		const float R = Size * 0.45f;

		if (bConnected)
		{
			Draw->AddCircleFilled(Centre, R, Col, 16);
		}
		else
		{
			Draw->AddCircle(Centre, R, Col, 16, 1.6f);
		}

		// Reserve cursor space for the icon + a small gap, then SameLine for
		// the label text that follows.
		ImGui::Dummy(ImVec2(Size, LineH));
		ImGui::SameLine(0.0f, 4.0f);
	}
}
