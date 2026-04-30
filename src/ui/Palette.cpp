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

		for (size_t I = 0; I < Registry.size(); ++I)
		{
			const FNodeRegistration& Reg = Registry[I];
			if (FilterBuffer[0] != '\0'
				&& !ContainsCaseInsensitive(Reg.MenuLabel, FilterBuffer)
				&& !ContainsCaseInsensitive(Reg.TypeName, FilterBuffer))
			{
				continue;
			}
			ImGui::PushID(static_cast<int32_t>(I));

			// Build the row as a single hovered/clickable block. We use Selectable
			// for the highlight + hover behaviour, then overdraw the icon and
			// label inside its rect via the window draw list.
			const ImVec2 RowStart = ImGui::GetCursorScreenPos();
			const float Width = ImGui::GetContentRegionAvail().x;

			// Icon + label are drawn on top via DrawList (not as ImGui items),
			// so no overlap-allow flag is needed.
			ImGui::Selectable("##palette_row", false, 0, ImVec2(Width, RowHeight));

			// Tooltip on hover — suppressed mid-drag so it doesn't overlap the
			// drag preview.
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

			// Drag source attaches to the Selectable above.
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
			{
				const int32_t Index = static_cast<int32_t>(I);
				ImGui::SetDragDropPayload(PaletteDragPayloadId, &Index, sizeof(Index));

				// Drag preview — same icon + label as the row.
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

			// Overdraw the icon and label on top of the Selectable's hit area.
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
	}
}
