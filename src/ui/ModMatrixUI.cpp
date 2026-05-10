#include "ui/ModMatrixUI.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "dsp/ModulationMatrix.h"
#include "graph/AudioCommand.h"
#include "graph/EditHistory.h"
#include "graph/Graph.h"

namespace NodeSynth
{
	namespace
	{
		// Column widths (pixels) — picked so "%.2f" formatted at -1.00 to +1.00
		// fits without truncation, plus comfortable padding either side.
		constexpr float ColLabelWidth = 36.0f;
		constexpr float ColCellWidth  = 56.0f;

		// Draw one bipolar depth/offset cell. Returns true if the value
		// changed this frame. The caller wraps with ImGui::PushID so the
		// underlying widget id is unique.
		bool BipolarCell(float& Value, bool bDimmed)
		{
			if (bDimmed)
			{
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);
			}
			ImGui::SetNextItemWidth(-FLT_MIN);  // fill the table cell
			const bool bChanged = ImGui::DragFloat("##cell", &Value, 0.005f,
				-1.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (bDimmed)
			{
				ImGui::PopStyleVar();
			}
			return bChanged;
		}
	}

	void DrawModMatrixUI(
		FModulationMatrix& Matrix,
		uint64_t NodeId,
		const FGraphModel& Model,
		const FCommandSink& Sink,
		FEditHistory* History)
	{
		ImGui::Separator();
		ImGui::TextDisabled("Modulation Matrix");
		ImGui::TextDisabled("Rows = destinations, columns = sources. Drag cells to scrub depth.");

		constexpr uint32_t Rows = FModulationMatrix::NumDestinations;
		constexpr uint32_t Cols = FModulationMatrix::NumSources;

		bool ColumnWired[Cols] = {};
		for (uint32_t J = 0; J < Cols; ++J)
		{
			ColumnWired[J] = Model.HasIncomingLink(NodeId, J);
		}

		// Pending edit-history capture (one entry per drag) — same pattern
		// as the standard widget loop in Editor.cpp's slider rendering.
		static uint64_t  ActiveNode = 0;
		static uint32_t  ActiveParam = 0;
		static float     ActiveOldValue = 0.0f;
		static bool      ActiveCaptured = false;

		auto WriteParam = [&](uint32_t ParamIndex, float NewValue)
		{
			Matrix.SetParamValue(ParamIndex, NewValue);
			Sink.SetParam(ParamIndex, NewValue);
		};

		auto BeginCapture = [&](uint32_t ParamIndex)
		{
			if (ImGui::IsItemActivated())
			{
				ActiveNode = NodeId;
				ActiveParam = ParamIndex;
				ActiveOldValue = Matrix.GetParamValue(ParamIndex);
				ActiveCaptured = true;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && ActiveCaptured
				&& ActiveNode == NodeId && ActiveParam == ParamIndex)
			{
				const float NewValue = Matrix.GetParamValue(ParamIndex);
				if (History != nullptr && ActiveOldValue != NewValue)
				{
					FEditCommand Cmd;
					Cmd.Type = EEditCommand::SetParam;
					Cmd.NodeId = NodeId;
					Cmd.ParamIndex = ParamIndex;
					Cmd.OldValue = ActiveOldValue;
					Cmd.NewValue = NewValue;
					History->Push(std::move(Cmd));
				}
				ActiveCaptured = false;
			}
		};

		auto ContextMenu = [&](uint32_t ParamIndex)
		{
			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Zero"))
				{
					const float Old = Matrix.GetParamValue(ParamIndex);
					WriteParam(ParamIndex, 0.0f);
					if (History != nullptr && Old != 0.0f)
					{
						FEditCommand Cmd;
						Cmd.Type = EEditCommand::SetParam;
						Cmd.NodeId = NodeId;
						Cmd.ParamIndex = ParamIndex;
						Cmd.OldValue = Old;
						Cmd.NewValue = 0.0f;
						History->Push(std::move(Cmd));
					}
				}
				if (ImGui::MenuItem("Invert"))
				{
					const float Old = Matrix.GetParamValue(ParamIndex);
					WriteParam(ParamIndex, -Old);
					if (History != nullptr && Old != 0.0f)
					{
						FEditCommand Cmd;
						Cmd.Type = EEditCommand::SetParam;
						Cmd.NodeId = NodeId;
						Cmd.ParamIndex = ParamIndex;
						Cmd.OldValue = Old;
						Cmd.NewValue = -Old;
						History->Push(std::move(Cmd));
					}
				}
				ImGui::EndPopup();
			}
		};

		// Total table width: row-label column + N source columns + a small
		// fudge factor for inner-cell padding. ImGui::BeginTable's outer
		// size of 0,0 means "fit content"; we set per-column widths so it
		// renders at exactly the right size.
		const ImGuiTableFlags TableFlags =
			ImGuiTableFlags_BordersInner |
			ImGuiTableFlags_SizingFixedFit |
			ImGuiTableFlags_NoHostExtendX;

		if (!ImGui::BeginTable("##mod_matrix", static_cast<int>(Cols) + 1, TableFlags))
		{
			return;
		}

		// Column setup: label column (narrow) + 8 source columns (wider).
		ImGui::TableSetupColumn("##rowlabel",
			ImGuiTableColumnFlags_WidthFixed, ColLabelWidth);
		for (uint32_t J = 0; J < Cols; ++J)
		{
			char ColId[16];
			std::snprintf(ColId, sizeof(ColId), "##src%u", J);
			ImGui::TableSetupColumn(ColId,
				ImGuiTableColumnFlags_WidthFixed, ColCellWidth);
		}

		// --- Header row: empty corner + Src labels (dimmed when unwired) ---
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		ImGui::TableSetColumnIndex(0);
		ImGui::TextDisabled(" ");
		for (uint32_t J = 0; J < Cols; ++J)
		{
			ImGui::TableSetColumnIndex(static_cast<int>(J) + 1);
			char Label[8];
			std::snprintf(Label, sizeof(Label), "Src%u", J + 1);
			// Centre the label within the column.
			const float Avail = ImGui::GetContentRegionAvail().x;
			const float TextW = ImGui::CalcTextSize(Label).x;
			const float Pad = std::max(0.0f, (Avail - TextW) * 0.5f);
			if (Pad > 0.0f)
			{
				ImGui::Dummy(ImVec2(Pad, 0.0f));
				ImGui::SameLine(0.0f, 0.0f);
			}
			if (ColumnWired[J])
			{
				ImGui::TextUnformatted(Label);
			}
			else
			{
				ImGui::TextDisabled("%s", Label);
			}
			if (!ColumnWired[J] && ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("No source connected to %s — depth column is inert.", Label);
			}
		}

		// --- Depth grid: one row per destination ---
		for (uint32_t I = 0; I < Rows; ++I)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("Dst%u", I + 1);
			for (uint32_t J = 0; J < Cols; ++J)
			{
				ImGui::TableSetColumnIndex(static_cast<int>(J) + 1);
				const uint32_t Idx = FModulationMatrix::DepthIndex(I, J);
				ImGui::PushID(static_cast<int>(Idx));
				float Value = Matrix.GetParamValue(Idx);
				if (BipolarCell(Value, !ColumnWired[J]))
				{
					WriteParam(Idx, Value);
				}
				BeginCapture(Idx);
				ContextMenu(Idx);
				ImGui::PopID();
			}
		}

		// --- Offset row at the bottom of the table ---
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextDisabled("Off");
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Per-destination DC offset added to each row's sum.");
		}
		for (uint32_t I = 0; I < Rows; ++I)
		{
			ImGui::TableSetColumnIndex(static_cast<int>(I) + 1);
			const uint32_t Idx = FModulationMatrix::OffsetIndex(I);
			ImGui::PushID(static_cast<int>(Idx));
			float Value = Matrix.GetParamValue(Idx);
			if (BipolarCell(Value, false))
			{
				WriteParam(Idx, Value);
			}
			BeginCapture(Idx);
			ContextMenu(Idx);
			ImGui::PopID();
		}

		ImGui::EndTable();
	}
}
