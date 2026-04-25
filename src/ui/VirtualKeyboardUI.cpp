#include "ui/VirtualKeyboardUI.h"

#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "dsp/VirtualKeyboard.h"

namespace NodeSynth
{
	namespace
	{
		// Semitone offsets from the displayed octave's bottom C.
		constexpr int32_t WhiteSemis[] = { 0, 2, 4, 5, 7, 9, 11, 12 };
		constexpr int32_t BlackSemis[] = { 1, 3, 6, 8, 10 };

		// Index of the white key that the corresponding black key sits to the right of.
		// Pattern in an octave: W B W B W . W B W B W B W (the dot is the gap between E and F).
		constexpr int32_t BlackAfterWhite[] = { 0, 1, 3, 4, 5 };

		// FL / Ableton-style computer-keyboard mapping: home row for whites, top row for blacks.
		ImGuiKey KeyForSemitone(int32_t Semi)
		{
			switch (Semi)
			{
				case 0:  return ImGuiKey_A;
				case 1:  return ImGuiKey_W;
				case 2:  return ImGuiKey_S;
				case 3:  return ImGuiKey_E;
				case 4:  return ImGuiKey_D;
				case 5:  return ImGuiKey_F;
				case 6:  return ImGuiKey_T;
				case 7:  return ImGuiKey_G;
				case 8:  return ImGuiKey_Y;
				case 9:  return ImGuiKey_H;
				case 10: return ImGuiKey_U;
				case 11: return ImGuiKey_J;
				case 12: return ImGuiKey_K;
				default: return ImGuiKey_None;
			}
		}

		const char* LabelForSemitone(int32_t Semi)
		{
			switch (Semi)
			{
				case 0:  return "A";
				case 1:  return "W";
				case 2:  return "S";
				case 3:  return "E";
				case 4:  return "D";
				case 5:  return "F";
				case 6:  return "T";
				case 7:  return "G";
				case 8:  return "Y";
				case 9:  return "H";
				case 10: return "U";
				case 11: return "J";
				case 12: return "K";
				default: return "";
			}
		}

		bool PointInRect(const ImVec2& P, const ImVec2& Min, const ImVec2& Max)
		{
			return P.x >= Min.x && P.x < Max.x && P.y >= Min.y && P.y < Max.y;
		}
	}

	void DrawVirtualKeyboardUI(FVirtualKeyboard& Kbd, const FCommandSink& Sink)
	{
		auto WriteParam = [&](uint32_t Index, float Value)
		{
			Kbd.SetParamValue(Index, Value);
			Sink.SetParam(Index, Value);
		};

		ImGui::Separator();

		// -- Octave row ---------------------------------------------------------
		int32_t Octave = static_cast<int32_t>(std::lround(Kbd.GetParamValue(FVirtualKeyboard::Param_Octave)));
		if (ImGui::Button("Oct -"))
		{
			WriteParam(FVirtualKeyboard::Param_Octave, static_cast<float>(Octave - 1));
		}
		ImGui::SameLine();
		ImGui::Text("C%d", Octave);
		ImGui::SameLine();
		if (ImGui::Button("Oct +"))
		{
			WriteParam(FVirtualKeyboard::Param_Octave, static_cast<float>(Octave + 1));
		}

		// Refresh in case the buttons changed it (also clamps).
		Octave = static_cast<int32_t>(std::lround(Kbd.GetParamValue(FVirtualKeyboard::Param_Octave)));

		// -- Mod slider ---------------------------------------------------------
		float Mod = Kbd.GetParamValue(FVirtualKeyboard::Param_ModWheel);
		if (ImGui::SliderFloat("Mod", &Mod, 0.0f, 1.0f, "%.2f"))
		{
			WriteParam(FVirtualKeyboard::Param_ModWheel, Mod);
		}

		// -- Piano keyboard -----------------------------------------------------
		// Sizes scale with the font so the keyboard grows on hi-DPI displays.
		// (GetFontSize() / 13 = the DPI scale set in main.cpp.)
		const float Scale = ImGui::GetFontSize() / 13.0f;
		const float WhiteKeyWidth = 28.0f * Scale;
		const float WhiteKeyHeight = 110.0f * Scale;
		const float BlackKeyWidth = 18.0f * Scale;
		const float BlackKeyHeight = 70.0f * Scale;
		constexpr int32_t NumWhiteKeys = 8;
		const float TotalWidth = NumWhiteKeys * WhiteKeyWidth;

		ImDrawList* Draw = ImGui::GetWindowDrawList();
		const ImVec2 Origin = ImGui::GetCursorScreenPos();

		// One InvisibleButton over the whole keyboard area to claim hover/active state.
		// Per-key hit-testing is done manually below so black keys can take priority.
		ImGui::InvisibleButton("##vkbd", ImVec2(TotalWidth, WhiteKeyHeight));
		const bool bAreaHovered = ImGui::IsItemHovered();
		const bool bMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

		auto WhiteKeyRect = [&](int32_t WhiteIndex, ImVec2& OutMin, ImVec2& OutMax)
		{
			OutMin = ImVec2(Origin.x + WhiteIndex * WhiteKeyWidth, Origin.y);
			OutMax = ImVec2(OutMin.x + WhiteKeyWidth, Origin.y + WhiteKeyHeight);
		};

		auto BlackKeyRect = [&](int32_t BlackIndex, ImVec2& OutMin, ImVec2& OutMax)
		{
			const int32_t WhiteIdx = BlackAfterWhite[BlackIndex];
			const float CenterX = Origin.x + (WhiteIdx + 1) * WhiteKeyWidth;
			OutMin = ImVec2(CenterX - BlackKeyWidth * 0.5f, Origin.y);
			OutMax = ImVec2(OutMin.x + BlackKeyWidth, Origin.y + BlackKeyHeight);
		};

		// -- Hit-test the mouse: black keys win when overlapping a white -------
		int32_t MouseSemi = -1;
		if (bAreaHovered)
		{
			const ImVec2 MousePos = ImGui::GetMousePos();
			for (size_t I = 0; I < std::size(BlackSemis); ++I)
			{
				ImVec2 Min;
				ImVec2 Max;
				BlackKeyRect(static_cast<int32_t>(I), Min, Max);
				if (PointInRect(MousePos, Min, Max))
				{
					MouseSemi = BlackSemis[I];
					break;
				}
			}
			if (MouseSemi < 0)
			{
				for (size_t I = 0; I < std::size(WhiteSemis); ++I)
				{
					ImVec2 Min;
					ImVec2 Max;
					WhiteKeyRect(static_cast<int32_t>(I), Min, Max);
					if (PointInRect(MousePos, Min, Max))
					{
						MouseSemi = WhiteSemis[I];
						break;
					}
				}
			}
		}

		// -- Build desired-held state from mouse + computer keyboard ------------
		bool DesiredHold[13] = {};

		if (bMouseDown && MouseSemi >= 0)
		{
			DesiredHold[MouseSemi] = true;
		}

		// Keyboard input is application-global so the user can hold a note on the
		// computer keyboard while dragging a slider in another window. We only
		// suppress when a text field is actively capturing typed input.
		const bool bAcceptKeys = !ImGui::GetIO().WantTextInput;
		if (bAcceptKeys)
		{
			for (int32_t Semi = 0; Semi <= 12; ++Semi)
			{
				const ImGuiKey Key = KeyForSemitone(Semi);
				if (Key != ImGuiKey_None && ImGui::IsKeyDown(Key))
				{
					DesiredHold[Semi] = true;
				}
			}
		}

		// -- Reconcile with actual held state -----------------------------------
		// If a text field grabs typing AND nothing is mouse-held, force-release
		// everything so notes don't stick when the user starts typing into a
		// slider's numeric edit. Mouse interaction always remains active.
		if (!bAcceptKeys && !bMouseDown)
		{
			Kbd.ReleaseAll();
		}
		else
		{
			for (int32_t Semi = 0; Semi <= 12; ++Semi)
			{
				const bool bIsHeld = Kbd.IsKeyHeld(Semi);
				if (DesiredHold[Semi] && !bIsHeld)
				{
					Kbd.PressNote(Semi);
				}
				else if (!DesiredHold[Semi] && bIsHeld)
				{
					Kbd.ReleaseNote(Semi);
				}
			}
		}

		// -- Draw white keys first (under the blacks) ---------------------------
		for (size_t I = 0; I < std::size(WhiteSemis); ++I)
		{
			ImVec2 Min;
			ImVec2 Max;
			WhiteKeyRect(static_cast<int32_t>(I), Min, Max);
			const int32_t Semi = WhiteSemis[I];
			const bool bHeld = Kbd.IsKeyHeld(Semi);
			const ImU32 Fill = bHeld
				? IM_COL32(180, 200, 255, 255)
				: IM_COL32(245, 245, 245, 255);
			Draw->AddRectFilled(Min, Max, Fill, 2.0f);
			Draw->AddRect(Min, Max, IM_COL32(0, 0, 0, 255), 2.0f, 0, 1.0f);

			const char* Label = LabelForSemitone(Semi);
			const ImVec2 TextSize = ImGui::CalcTextSize(Label);
			const ImVec2 TextPos(
				Min.x + ((Max.x - Min.x) - TextSize.x) * 0.5f,
				Max.y - TextSize.y - 6.0f);
			Draw->AddText(TextPos, IM_COL32(40, 40, 40, 255), Label);
		}

		// -- Draw black keys on top ---------------------------------------------
		for (size_t I = 0; I < std::size(BlackSemis); ++I)
		{
			ImVec2 Min;
			ImVec2 Max;
			BlackKeyRect(static_cast<int32_t>(I), Min, Max);
			const int32_t Semi = BlackSemis[I];
			const bool bHeld = Kbd.IsKeyHeld(Semi);
			const ImU32 Fill = bHeld
				? IM_COL32(120, 80, 200, 255)
				: IM_COL32(20, 20, 20, 255);
			Draw->AddRectFilled(Min, Max, Fill, 2.0f);
			Draw->AddRect(Min, Max, IM_COL32(0, 0, 0, 255), 2.0f, 0, 1.0f);

			const char* Label = LabelForSemitone(Semi);
			const ImVec2 TextSize = ImGui::CalcTextSize(Label);
			const ImVec2 TextPos(
				Min.x + ((Max.x - Min.x) - TextSize.x) * 0.5f,
				Max.y - TextSize.y - 4.0f);
			Draw->AddText(TextPos, IM_COL32(220, 220, 220, 255), Label);
		}
	}
}
