#include "ui/Palette.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#include <imgui.h>

#include "ui/NodeIcons.h"
#include "ui/NodeRegistry.h"

namespace NodeSynth
{
	namespace
	{
		// Case-insensitive substring match. Used by the palette filter.
		bool ContainsCaseInsensitive(const char* Haystack, const char* Needle)
		{
			if (Needle == nullptr || *Needle == '\0') { return true; }
			if (Haystack == nullptr) { return false; }
			const size_t HLen = std::strlen(Haystack);
			const size_t NLen = std::strlen(Needle);
			if (NLen > HLen) { return false; }
			for (size_t I = 0; I <= HLen - NLen; ++I)
			{
				size_t J = 0;
				while (J < NLen)
				{
					const char A = static_cast<char>(std::tolower(Haystack[I + J]));
					const char B = static_cast<char>(std::tolower(Needle[J]));
					if (A != B) { break; }
					++J;
				}
				if (J == NLen) { return true; }
			}
			return false;
		}
	}

	namespace
	{
		// Renders a single palette row (icon + label, draggable). Factored
		// out so we can call it from both the grouped and the flat-search
		// passes without duplicating the drag-drop / tooltip plumbing.
		void DrawPaletteRow(size_t Index, const FNodeRegistration& Reg,
			float IconSize, float RowHeight, float Padding)
		{
			ImGui::PushID(static_cast<int32_t>(Index));

			const ImVec2 RowStart = ImGui::GetCursorScreenPos();
			const float Width = ImGui::GetContentRegionAvail().x;
			ImGui::Selectable("##palette_row", false, 0, ImVec2(Width, RowHeight));

			if (Reg.Description != nullptr
				&& ImGui::IsItemHovered()
				&& ImGui::GetDragDropPayload() == nullptr)
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(Reg.MenuLabel);
				ImGui::Separator();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
				ImGui::TextUnformatted(Reg.Description);
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}

			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
			{
				const int32_t Idx = static_cast<int32_t>(Index);
				ImGui::SetDragDropPayload(PaletteDragPayloadId, &Idx, sizeof(Idx));

				const float PreviewIconSize = IconSize;
				const ImVec2 PreviewIconMin = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(PreviewIconSize, PreviewIconSize));
				DrawNodeIcon(Reg.TypeName, ImGui::GetWindowDrawList(),
					PreviewIconMin,
					ImVec2(PreviewIconMin.x + PreviewIconSize, PreviewIconMin.y + PreviewIconSize));
				ImGui::SameLine();
				ImGui::TextUnformatted(Reg.MenuLabel);

				ImGui::EndDragDropSource();
			}

			ImDrawList* Draw = ImGui::GetWindowDrawList();
			const ImVec2 IconMin(RowStart.x + Padding, RowStart.y + 2.0f);
			const ImVec2 IconMax(IconMin.x + IconSize, IconMin.y + IconSize);
			DrawNodeIcon(Reg.TypeName, Draw, IconMin, IconMax);

			const ImVec2 TextSize = ImGui::CalcTextSize(Reg.MenuLabel);
			const ImVec2 TextPos(IconMax.x + 8.0f,
				RowStart.y + (RowHeight - TextSize.y) * 0.5f);
			Draw->AddText(TextPos, ImGui::GetColorU32(ImGuiCol_Text), Reg.MenuLabel);

			ImGui::PopID();
		}

		bool MatchesFilter(const FNodeRegistration& Reg, const char* Filter)
		{
			if (Filter == nullptr || Filter[0] == '\0') { return true; }
			return ContainsCaseInsensitive(Reg.MenuLabel, Filter)
				|| ContainsCaseInsensitive(Reg.TypeName, Filter)
				|| ContainsCaseInsensitive(Reg.Category, Filter);
		}
	}

	void DrawNodePalette()
	{
		ImGui::TextDisabled("Drag a node into the graph.");

		// Filter input. Persistent state via ImGui's static buffer pattern.
		static char FilterBuffer[64] = "";
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##palette_filter", "Filter...",
			FilterBuffer, sizeof(FilterBuffer));
		ImGui::Separator();

		const auto& Registry = GetNodeRegistry();
		const float IconSize = ImGui::GetTextLineHeight() * 2.4f;
		const float RowHeight = IconSize + 6.0f;
		const float Padding = 6.0f;
		const bool bHasFilter = (FilterBuffer[0] != '\0');

		// With an active filter we render a flat list so matches don't get
		// hidden behind a collapsed header. Without a filter we group by
		// Category under collapsible headers (registry is pre-ordered so a
		// linear walk emits them in the right sequence).
		if (bHasFilter)
		{
			for (size_t I = 0; I < Registry.size(); ++I)
			{
				const FNodeRegistration& Reg = Registry[I];
				if (!MatchesFilter(Reg, FilterBuffer)) { continue; }
				DrawPaletteRow(I, Reg, IconSize, RowHeight, Padding);
			}
			return;
		}

		const char* CurrentCategory = nullptr;
		bool bSectionOpen = false;
		for (size_t I = 0; I < Registry.size(); ++I)
		{
			const FNodeRegistration& Reg = Registry[I];
			const char* Cat = (Reg.Category != nullptr) ? Reg.Category : "Other";

			// Emit a header whenever the category changes.
			if (CurrentCategory == nullptr || std::strcmp(Cat, CurrentCategory) != 0)
			{
				CurrentCategory = Cat;
				ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
				bSectionOpen = ImGui::CollapsingHeader(Cat);
			}

			if (!bSectionOpen) { continue; }
			DrawPaletteRow(I, Reg, IconSize, RowHeight, Padding);
		}
	}
}
