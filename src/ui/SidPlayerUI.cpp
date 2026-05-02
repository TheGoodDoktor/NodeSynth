#include "ui/SidPlayerUI.h"

#include <imgui.h>

#include "dsp/SidPlayer.h"

namespace NodeSynth
{
	void DrawSidPlayerUI(FSidPlayer& Player)
	{
		const FSidPlayer::FLoadStatus Status = Player.GetStatus();

		ImGui::Separator();
		ImGui::TextDisabled("SID Status");

		if (!Status.ErrorMessage.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 80, 80, 255));
			ImGui::TextWrapped("Load failed: %s", Status.ErrorMessage.c_str());
			ImGui::PopStyleColor();
			return;
		}

		if (!Status.bLoaded)
		{
			ImGui::TextDisabled("(no file loaded)");
			return;
		}

		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 220, 120, 255));
		ImGui::TextUnformatted("Loaded");
		ImGui::PopStyleColor();

		// Header fields. Empty strings are skipped so a tune missing one
		// of the optional metadata blocks doesn't render a dangling row.
		if (!Status.TuneName.empty())
		{
			ImGui::Text("Name:     %s", Status.TuneName.c_str());
		}
		if (!Status.Author.empty())
		{
			ImGui::Text("Author:   %s", Status.Author.c_str());
		}
		if (!Status.Released.empty())
		{
			ImGui::Text("Released: %s", Status.Released.c_str());
		}
		if (Status.Songs > 0)
		{
			ImGui::Text("Subtunes: %u (start = %u)",
				static_cast<unsigned>(Status.Songs),
				static_cast<unsigned>(Status.StartSong));
		}
		ImGui::Text("Region:   %s", Status.bIsNtsc ? "NTSC" : "PAL");
		ImGui::Text("Model:    %s", Status.bIs8580 ? "8580" : "6581");

		// Live per-voice gate indicators. Lights up green when the SID's
		// V*n*_Gate bit is set; gray when off. Useful as a "is it actually
		// playing" sanity check — silence with all three lit usually means
		// the audio output isn't wired into the graph.
		ImGui::Spacing();
		ImGui::TextDisabled("Gates:");
		ImGui::SameLine();
		const ImVec4 OnColor(0.30f, 0.85f, 0.30f, 1.0f);
		const ImVec4 OffColor(0.20f, 0.20f, 0.20f, 1.0f);
		const ImVec2 LedSize(14.0f, 14.0f);
		for (uint32_t V = 0; V < 3; ++V)
		{
			const bool bOn = Player.GetVoiceGate(V);
			ImGui::PushID(static_cast<int>(V));
			ImGui::ColorButton("##gate", bOn ? OnColor : OffColor,
				ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, LedSize);
			ImGui::PopID();
			if (V < 2) { ImGui::SameLine(); }
		}
	}
}
