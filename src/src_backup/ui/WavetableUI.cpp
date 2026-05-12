#include "ui/WavetableUI.h"

#include <imgui.h>
#include <nfd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "dsp/Wavetable.h"
#include "dsp/WavetableOscillator.h"
#include "io/WavetableBrowser.h"

namespace NodeSynth
{
	namespace
	{
		// Cached library — built on first use, refreshed by the
		// "Refresh" button. Static so all wavetable nodes share a single
		// scan; the bundled+user dirs don't change at runtime.
		FWavetableLibrary& Library()
		{
			static FWavetableLibrary L = BuildWavetableLibrary(
				GetBundledWavetableDir(), GetUserWavetableDir());
			return L;
		}

		void RefreshLibrary()
		{
			Library() = BuildWavetableLibrary(
				GetBundledWavetableDir(), GetUserWavetableDir());
		}

		// Format the dropdown's combo-preview string from the current
		// stored path. Falls back to "(none)" when unset and "(missing)"
		// when the path is set but the WT didn't load.
		std::string PreviewLabel(const std::string& Stored, bool bLoaded)
		{
			if (Stored.empty()) { return "(none)"; }
			const std::filesystem::path P(Stored);
			const std::string Stem = P.stem().string();
			if (Stem.empty()) { return "(invalid)"; }
			return bLoaded ? Stem : (Stem + " (missing)");
		}
	}

	void DrawWavetableUI(FWavetableOscillator& Wt)
	{
		ImGui::Separator();
		ImGui::TextDisabled("Wavetable");

		const std::string Stored = Wt.GetParamString(FWavetableOscillator::Param_Wavetable);
		const std::shared_ptr<FWavetableData> Table = Wt.GetCurrentTable();
		const bool bLoaded = (Table != nullptr) && !Table->Frames.empty();
		const std::string Preview = PreviewLabel(Stored, bLoaded);

		// Library dropdown. Categories shown as disabled headers; entries
		// as selectables. Picking writes the relative path so the choice
		// is portable across machines.
		ImGui::SetNextItemWidth(-FLT_MIN - 70.0f);
		if (ImGui::BeginCombo("##wt_picker", Preview.c_str()))
		{
			const FWavetableLibrary& Lib = Library();
			if (Lib.IsEmpty())
			{
				ImGui::TextDisabled("(no wavetables found)");
			}
			for (const FWavetableCategory& Cat : Lib.Categories)
			{
				if (!Cat.Name.empty())
				{
					ImGui::Spacing();
					ImGui::TextDisabled("%s", Cat.Name.c_str());
				}
				for (const FWavetableEntry& E : Cat.Entries)
				{
					const std::string RelStr = E.RelativePath.generic_string();
					const bool bSelected = (Stored == RelStr);
					ImGui::PushID(RelStr.c_str());
					if (ImGui::Selectable(E.DisplayName.c_str(), bSelected))
					{
						Wt.SetParamString(FWavetableOscillator::Param_Wavetable, RelStr);
					}
					if (bSelected) { ImGui::SetItemDefaultFocus(); }
					ImGui::PopID();
				}
			}
			ImGui::EndCombo();
		}

		// "..." button — for picking a .wav outside the bundled / user
		// directories. Stores an absolute path; the patch won't be
		// portable but the user can still audition arbitrary files.
		ImGui::SameLine();
		if (ImGui::SmallButton("..."))
		{
			nfdu8char_t* OutPath = nullptr;
			nfdu8filteritem_t Filter[1] = { { "Wavetable WAV", "wav" } };
			nfdopendialogu8args_t Args = {};
			Args.filterList = Filter;
			Args.filterCount = 1;
			const nfdresult_t Result = NFD_OpenDialogU8_With(&OutPath, &Args);
			if (Result == NFD_OKAY && OutPath)
			{
				Wt.SetParamString(FWavetableOscillator::Param_Wavetable, OutPath);
				NFD_FreePathU8(OutPath);
			}
		}

		ImGui::SameLine();
		if (ImGui::SmallButton("Refresh"))
		{
			RefreshLibrary();
		}

		// Frame preview. Show the frame closest to the current Position
		// param so the user sees what's currently being played at the
		// integer-frame midpoint. Position-modulating sources (LFO,
		// ADSR) aren't reflected here — that would need access to the
		// audio thread's last-rendered Position which we don't track.
		if (bLoaded)
		{
			const float Pos = Wt.GetParamValue(FWavetableOscillator::Param_Position);
			const uint32_t NumFrames = Table->NumFrames();
			const float MaxIdx = (NumFrames > 1)
				? static_cast<float>(NumFrames - 1) : 0.0f;
			const uint32_t FrameIdx = std::min<uint32_t>(NumFrames - 1,
				static_cast<uint32_t>(std::round(Pos * MaxIdx)));
			const std::vector<float>& Samples = Table->Frames[FrameIdx].Mips[0];
			ImGui::PlotLines("##wt_preview", Samples.data(),
				static_cast<int>(Samples.size()), 0, nullptr,
				-1.1f, 1.1f, ImVec2(-FLT_MIN, 80.0f));
			ImGui::Text("Frame %u / %u", FrameIdx + 1u, NumFrames);
		}
		else if (!Stored.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 120, 120, 255));
			ImGui::TextWrapped("Failed to load: %s", Stored.c_str());
			ImGui::PopStyleColor();
		}
	}
}
