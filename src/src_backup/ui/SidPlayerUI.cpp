#include "ui/SidPlayerUI.h"

#include <imgui.h>
#include <nfd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "dsp/SidPlayer.h"
#include "io/SidBrowser.h"

namespace NodeSynth
{
	namespace
	{
		// Cached library, scanned on first use. Refresh button (rare)
		// rescans. Bundled + user dirs don't change at runtime.
		FSidLibrary& Library()
		{
			static FSidLibrary L = BuildSidLibrary(GetBundledSidDir(), GetUserSidDir());
			return L;
		}

		void RefreshLibrary()
		{
			Library() = BuildSidLibrary(GetBundledSidDir(), GetUserSidDir());
		}

		std::string PreviewLabel(const std::string& Stored)
		{
			if (Stored.empty()) { return "(none)"; }
			const std::filesystem::path P(Stored);
			const std::string Stem = P.stem().string();
			return Stem.empty() ? std::string("(invalid)") : Stem;
		}
	}

	void DrawSidPlayerUI(FSidPlayer& Player)
	{
		// --- Tune picker dropdown ----------------------------------------
		const std::string Stored = Player.GetParamString(FSidPlayer::Param_File);
		const std::string Preview = PreviewLabel(Stored);

		ImGui::TextDisabled("SID Tune");
		ImGui::SetNextItemWidth(-FLT_MIN - 70.0f);
		if (ImGui::BeginCombo("##sid_picker", Preview.c_str()))
		{
			const FSidLibrary& Lib = Library();
			if (Lib.IsEmpty())
			{
				ImGui::TextDisabled("(no .sid files in sidfiles/)");
			}
			for (const FSidCategory& Cat : Lib.Categories)
			{
				if (!Cat.Name.empty())
				{
					ImGui::Spacing();
					ImGui::TextDisabled("%s", Cat.Name.c_str());
				}
				for (const FSidEntry& E : Cat.Entries)
				{
					const std::string RelStr = E.RelativePath.generic_string();
					const bool bSelected = (Stored == RelStr);
					ImGui::PushID(RelStr.c_str());
					if (ImGui::Selectable(E.DisplayName.c_str(), bSelected))
					{
						Player.SetParamString(FSidPlayer::Param_File, RelStr);
					}
					if (bSelected) { ImGui::SetItemDefaultFocus(); }
					ImGui::PopID();
				}
			}
			ImGui::EndCombo();
		}

		// "..." browse button — pick a .sid outside the bundled dirs.
		// Stores an absolute path; the patch won't be portable but the
		// user can still audition arbitrary files.
		ImGui::SameLine();
		if (ImGui::SmallButton("..."))
		{
			nfdu8char_t* OutPath = nullptr;
			nfdu8filteritem_t Filter[1] = { { "SID Tune", "sid" } };
			nfdopendialogu8args_t Args = {};
			Args.filterList = Filter;
			Args.filterCount = 1;
			const nfdresult_t Result = NFD_OpenDialogU8_With(&OutPath, &Args);
			if (Result == NFD_OKAY && OutPath)
			{
				Player.SetParamString(FSidPlayer::Param_File, OutPath);
				NFD_FreePathU8(OutPath);
			}
		}

		ImGui::SameLine();
		if (ImGui::SmallButton("Refresh"))
		{
			RefreshLibrary();
		}

		// --- Status (existing block) -------------------------------------
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

		// Live per-voice gate indicators.
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
