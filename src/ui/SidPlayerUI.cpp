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
	}
}
