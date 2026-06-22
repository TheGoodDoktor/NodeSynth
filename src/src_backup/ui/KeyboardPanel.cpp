#include "ui/KeyboardPanel.h"

#include <cmath>

#include <imgui.h>

namespace NodeSynth
{
	namespace
	{
		// Semitone offsets from the displayed octave's bottom C.
		constexpr int32_t WhiteSemis[] = { 0, 2, 4, 5, 7, 9, 11, 12 };
		constexpr int32_t BlackSemis[] = { 1, 3, 6, 8, 10 };
		// Index of the white key that the corresponding black key sits to the right of.
		constexpr int32_t BlackAfterWhite[] = { 0, 1, 3, 4, 5 };

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

		uint8_t SemitoneToMidi(int32_t Octave, int32_t Semi)
		{
			const int32_t M = 12 * (Octave + 1) + Semi;
			if (M < 0) { return 0; }
			if (M > 127) { return 127; }
			return static_cast<uint8_t>(M);
		}
	}

	bool FKeyboardPanel::IsKeyHeld(int32_t SemitoneFromBottomC) const
	{
		for (size_t I = 0; I < NumHeldNotes; ++I)
		{
			if (HeldSemitones[I] == static_cast<int8_t>(SemitoneFromBottomC))
			{
				return true;
			}
		}
		return false;
	}

	void FKeyboardPanel::PressNote(int32_t Semi, const FCommandSink& Sink)
	{
		const uint8_t Note = SemitoneToMidi(Octave.load(std::memory_order_relaxed), Semi);
		for (size_t I = 0; I < NumHeldNotes; ++I)
		{
			if (HeldNotes[I] == Note) { return; }
		}
		if (NumHeldNotes >= MaxHeldNotes) { return; }
		HeldNotes[NumHeldNotes] = Note;
		HeldSemitones[NumHeldNotes] = static_cast<int8_t>(Semi);
		++NumHeldNotes;
		Sink.NoteOn(Note, Velocity.load(std::memory_order_relaxed));
	}

	void FKeyboardPanel::ReleaseNote(int32_t Semi, const FCommandSink& Sink)
	{
		for (size_t I = 0; I < NumHeldNotes; ++I)
		{
			if (HeldSemitones[I] == static_cast<int8_t>(Semi))
			{
				const uint8_t Note = HeldNotes[I];
				for (size_t J = I + 1; J < NumHeldNotes; ++J)
				{
					HeldNotes[J - 1] = HeldNotes[J];
					HeldSemitones[J - 1] = HeldSemitones[J];
				}
				--NumHeldNotes;
				Sink.NoteOff(Note);
				return;
			}
		}
	}

	void FKeyboardPanel::ReleaseAll(const FCommandSink& Sink)
	{
		for (size_t I = 0; I < NumHeldNotes; ++I)
		{
			Sink.NoteOff(HeldNotes[I]);
		}
		NumHeldNotes = 0;
	}

	void FKeyboardPanel::Draw(const FCommandSink& Sink)
	{
		// -- Octave + Velocity controls ----------------------------------------
		int32_t Oct = Octave.load(std::memory_order_relaxed);
		if (ImGui::Button("Oct -")) { Oct = std::max(0, Oct - 1); Octave.store(Oct); }
		ImGui::SameLine();
		ImGui::Text("C%d", Oct);
		ImGui::SameLine();
		if (ImGui::Button("Oct +")) { Oct = std::min(8, Oct + 1); Octave.store(Oct); }
		ImGui::SameLine();
		float Vel = Velocity.load(std::memory_order_relaxed);
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::SliderFloat("Vel", &Vel, 0.0f, 1.0f, "%.2f"))
		{
			if (Vel < 0.0f) { Vel = 0.0f; }
			if (Vel > 1.0f) { Vel = 1.0f; }
			Velocity.store(Vel, std::memory_order_relaxed);
		}

		// -- Piano keyboard ----------------------------------------------------
		const float Scale = ImGui::GetFontSize() / 13.0f;
		const float WhiteKeyWidth = 28.0f * Scale;
		const float WhiteKeyHeight = 110.0f * Scale;
		const float BlackKeyWidth = 18.0f * Scale;
		const float BlackKeyHeight = 70.0f * Scale;
		constexpr int32_t NumWhiteKeys = 8;
		const float TotalWidth = NumWhiteKeys * WhiteKeyWidth;

		ImDrawList* Draw = ImGui::GetWindowDrawList();
		const ImVec2 Origin = ImGui::GetCursorScreenPos();

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

		// Hit-test mouse.
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

		// Build desired-held state from mouse + computer keyboard.
		bool DesiredHold[13] = {};
		if (bMouseDown && MouseSemi >= 0)
		{
			DesiredHold[MouseSemi] = true;
		}
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

		// Reconcile.
		if (!bAcceptKeys && !bMouseDown)
		{
			ReleaseAll(Sink);
		}
		else
		{
			for (int32_t Semi = 0; Semi <= 12; ++Semi)
			{
				const bool bIsHeld = IsKeyHeld(Semi);
				if (DesiredHold[Semi] && !bIsHeld) { PressNote(Semi, Sink); }
				else if (!DesiredHold[Semi] && bIsHeld) { ReleaseNote(Semi, Sink); }
			}
		}

		// Draw white keys.
		for (size_t I = 0; I < std::size(WhiteSemis); ++I)
		{
			ImVec2 Min;
			ImVec2 Max;
			WhiteKeyRect(static_cast<int32_t>(I), Min, Max);
			const int32_t Semi = WhiteSemis[I];
			const bool bHeld = IsKeyHeld(Semi);
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

		// Draw black keys.
		for (size_t I = 0; I < std::size(BlackSemis); ++I)
		{
			ImVec2 Min;
			ImVec2 Max;
			BlackKeyRect(static_cast<int32_t>(I), Min, Max);
			const int32_t Semi = BlackSemis[I];
			const bool bHeld = IsKeyHeld(Semi);
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
