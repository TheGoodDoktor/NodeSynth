#include "ui/Editor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor.h>

#include "dsp/Adsr.h"
#include "dsp/Gain.h"
#include "dsp/GateButton.h"
#include "dsp/MidiInput.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/Svf.h"
#include "dsp/Vca.h"
#include "dsp/VirtualKeyboard.h"
#include "ui/AdsrUI.h"
#include "ui/NodeIcons.h"
#include "ui/NodeRegistry.h"
#include "ui/Palette.h"
#include "ui/VirtualKeyboardUI.h"

namespace ed = ax::NodeEditor;

namespace NodeSynth
{
	namespace
	{
		// Pin id layout: [NodeId (high 48 bits) | PortIndex (bits 1..15) | IsOutput (bit 0)].
		constexpr uint64_t NodeIdShift = 16;
		constexpr uint64_t PortIndexShift = 1;
		constexpr uint64_t OutputBit = 0x1ULL;
		constexpr uint64_t PortIndexMask = 0x7FFFULL; // 15 bits

		uint64_t EncodePinId(FNodeId NodeId, uint32_t PortIndex, bool bIsOutput)
		{
			return (NodeId << NodeIdShift)
				| ((static_cast<uint64_t>(PortIndex) & PortIndexMask) << PortIndexShift)
				| (bIsOutput ? OutputBit : 0ULL);
		}

		void DecodePinId(uint64_t PinId, FNodeId& OutNodeId, uint32_t& OutPortIndex, bool& bOutIsOutput)
		{
			bOutIsOutput = (PinId & OutputBit) != 0;
			OutPortIndex = static_cast<uint32_t>((PinId >> PortIndexShift) & PortIndexMask);
			OutNodeId = PinId >> NodeIdShift;
		}
	}

	FGraphEditorPanel::FGraphEditorPanel(std::string InSettingsFile)
		: SettingsFilePath(std::move(InSettingsFile))
	{
		ed::Config EditorConfig;
		EditorConfig.SettingsFile = SettingsFilePath.empty()
			? nullptr
			: SettingsFilePath.c_str();
		Context = ed::CreateEditor(&EditorConfig);
	}

	FGraphEditorPanel::~FGraphEditorPanel()
	{
		if (Context)
		{
			ed::DestroyEditor(Context);
			Context = nullptr;
		}
	}

	bool FGraphEditorPanel::Draw(FGraphModel& Model)
	{
		bool bChanged = false;

		ed::SetCurrentEditor(Context);
		ed::Begin("Node Editor", ImVec2(0.0f, 0.0f));

		// Draw nodes.
		for (const auto& [Id, Rec] : Model.GetNodes())
		{
			const INode& Node = *Rec.Node;
			const auto InPorts = Node.GetInputPorts();
			const auto OutPorts = Node.GetOutputPorts();

			ed::BeginNode(ed::NodeId(Id));
			IconBeforeText(Node.GetTypeName(), ImGui::GetTextLineHeight());
			ImGui::TextUnformatted(Node.GetTypeName());
			if (Rec.bPerVoice)
			{
				// Per-voice badge: small bracketed label after the type name so
				// it's obvious at a glance which nodes will be cloned per voice.
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "[poly]");
			}
			ImGui::Dummy(ImVec2(120.0f, 2.0f));

			const size_t Rows = std::max(InPorts.size(), OutPorts.size());
			for (size_t Row = 0; Row < Rows; ++Row)
			{
				if (Row < InPorts.size())
				{
					ed::BeginPin(ed::PinId(EncodePinId(Id, static_cast<uint32_t>(Row), false)), ed::PinKind::Input);
					ImGui::Text("-> %s", InPorts[Row].Name.c_str());
					ed::EndPin();
				}
				else
				{
					ImGui::Dummy(ImVec2(60.0f, ImGui::GetTextLineHeight()));
				}

				if (Row < OutPorts.size())
				{
					ImGui::SameLine(120.0f);
					ed::BeginPin(ed::PinId(EncodePinId(Id, static_cast<uint32_t>(Row), true)), ed::PinKind::Output);
					ImGui::Text("%s ->", OutPorts[Row].Name.c_str());
					ed::EndPin();
				}
			}
			ed::EndNode();

			if (bFirstFrame)
			{
				ed::SetNodePosition(ed::NodeId(Id), ImVec2(Rec.PositionX, Rec.PositionY));
			}
		}

		// Draw links.
		for (const FLink& L : Model.GetLinks())
		{
			ed::Link(ed::LinkId(L.Id),
				ed::PinId(EncodePinId(L.FromNode, L.FromPort, true)),
				ed::PinId(EncodePinId(L.ToNode, L.ToPort, false)));
		}

		// Creation: user drags a link between two pins.
		if (ed::BeginCreate())
		{
			ed::PinId FromPin;
			ed::PinId ToPin;
			if (ed::QueryNewLink(&FromPin, &ToPin))
			{
				if (FromPin && ToPin)
				{
					FNodeId FromNode = 0;
					FNodeId ToNode = 0;
					uint32_t FromPort = 0;
					uint32_t ToPort = 0;
					bool bFromIsOutput = false;
					bool bToIsOutput = false;
					DecodePinId(FromPin.Get(), FromNode, FromPort, bFromIsOutput);
					DecodePinId(ToPin.Get(), ToNode, ToPort, bToIsOutput);

					// imgui-node-editor lets the user drag either direction; normalise.
					if (bFromIsOutput == bToIsOutput)
					{
						ed::RejectNewItem(ImColor(255, 64, 64), 2.0f);
					}
					else
					{
						if (!bFromIsOutput)
						{
							std::swap(FromNode, ToNode);
							std::swap(FromPort, ToPort);
						}
						if (ed::AcceptNewItem())
						{
							if (Model.AddLink(FromNode, FromPort, ToNode, ToPort) != 0)
							{
								bChanged = true;
							}
						}
					}
				}
			}
		}
		ed::EndCreate();

		// Deletion: user presses Delete with links/nodes selected.
		if (ed::BeginDelete())
		{
			ed::LinkId DeletedLink;
			while (ed::QueryDeletedLink(&DeletedLink))
			{
				if (ed::AcceptDeletedItem())
				{
					Model.RemoveLink(DeletedLink.Get());
					bChanged = true;
				}
			}
			ed::NodeId DeletedNode;
			while (ed::QueryDeletedNode(&DeletedNode))
			{
				if (ed::AcceptDeletedItem())
				{
					Model.RemoveNode(DeletedNode.Get());
					bChanged = true;
				}
			}
		}
		ed::EndDelete();

		// Right-click background for the create-node menu.
		ed::Suspend();
		if (ed::ShowBackgroundContextMenu())
		{
			ImGui::OpenPopup("CreateNodeMenu");
		}
		ed::Resume();

		// Right-click on a node — per-voice toggle and friends.
		ed::NodeId NodeContextId;
		ed::Suspend();
		if (ed::ShowNodeContextMenu(&NodeContextId))
		{
			NodeContextTarget = NodeContextId.Get();
			ImGui::OpenPopup("NodeContextMenu");
		}
		ed::Resume();

		ed::Suspend();
		if (ImGui::BeginPopup("NodeContextMenu"))
		{
			if (FNodeRecord* Rec = Model.FindNode(NodeContextTarget))
			{
				const bool bCloneable =
					Rec->Node && Rec->Node->Clone() != nullptr;
				bool bPoly = Rec->bPerVoice;
				if (ImGui::MenuItem("Per-voice", nullptr, &bPoly, bCloneable))
				{
					if (Model.SetNodePerVoice(NodeContextTarget, bPoly))
					{
						bChanged = true;
					}
				}
				if (!bCloneable)
				{
					ImGui::TextDisabled("(this node type can't be cloned)");
				}
			}
			ImGui::EndPopup();
		}
		ed::Resume();

		ed::Suspend();
		if (ImGui::BeginPopup("CreateNodeMenu"))
		{
			const ImVec2 MouseScreen = ImGui::GetMousePosOnOpeningCurrentPopup();
			const ImVec2 MouseCanvas = ed::ScreenToCanvas(MouseScreen);

			auto SpawnNode = [&](std::shared_ptr<INode> NewNode)
			{
				const FNodeId NewId = Model.AddNode(NewNode, MouseCanvas.x, MouseCanvas.y);
				ed::SetNodePosition(ed::NodeId(NewId), MouseCanvas);
				bChanged = true;
			};

			for (const FNodeRegistration& Reg : GetNodeRegistry())
			{
				IconBeforeText(Reg.TypeName, ImGui::GetTextLineHeight());
				if (ImGui::MenuItem(Reg.MenuLabel))
				{
					SpawnNode(Reg.Make());
				}
			}
			ImGui::EndPopup();
		}
		ed::Resume();

		// Hover tooltip on graph nodes. Looks up the same registry entry the
		// palette uses so the wording stays in sync. Suspended out of the editor
		// canvas so it renders in screen space; suppressed during drag-drop so
		// it doesn't fight the drag preview.
		if (const ed::NodeId HoveredNode = ed::GetHoveredNode())
		{
			if (FNodeRecord* Rec = Model.FindNode(HoveredNode.Get());
				Rec != nullptr && ImGui::GetDragDropPayload() == nullptr)
			{
				const char* TypeName = Rec->Node->GetTypeName();
				const FNodeRegistration* Match = nullptr;
				for (const FNodeRegistration& Reg : GetNodeRegistry())
				{
					if (std::strcmp(Reg.TypeName, TypeName) == 0)
					{
						Match = &Reg;
						break;
					}
				}
				if (Match != nullptr && Match->Description != nullptr)
				{
					ed::Suspend();
					ImGui::BeginTooltip();
					ImGui::TextUnformatted(Match->MenuLabel);
					ImGui::Separator();
					ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
					ImGui::TextUnformatted(Match->Description);
					ImGui::PopTextWrapPos();
					ImGui::EndTooltip();
					ed::Resume();
				}
			}
		}

		ed::End();

		// Drop target for the node palette. Covers the entire graph window so the
		// user can drop a palette entry anywhere in the canvas. ScreenToCanvas
		// requires the editor context to still be current, hence ahead of
		// SetCurrentEditor(nullptr).
		const ImVec2 WinPos = ImGui::GetWindowPos();
		const ImVec2 WinSize = ImGui::GetWindowSize();
		const ImRect DropRect(WinPos, ImVec2(WinPos.x + WinSize.x, WinPos.y + WinSize.y));
		if (ImGui::BeginDragDropTargetCustom(DropRect, ImGui::GetID("##graph_drop")))
		{
			if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(PaletteDragPayloadId))
			{
				const int32_t Index = *static_cast<const int32_t*>(Payload->Data);
				const auto& Registry = GetNodeRegistry();
				if (Index >= 0 && Index < static_cast<int32_t>(Registry.size()))
				{
					const ImVec2 ScreenPos = ImGui::GetMousePos();
					const ImVec2 CanvasPos = ed::ScreenToCanvas(ScreenPos);
					std::shared_ptr<INode> NewNode = Registry[Index].Make();
					const FNodeId NewId = Model.AddNode(NewNode, CanvasPos.x, CanvasPos.y);
					ed::SetNodePosition(ed::NodeId(NewId), CanvasPos);
					bChanged = true;
				}
			}
			ImGui::EndDragDropTarget();
		}

		ed::SetCurrentEditor(nullptr);

		bFirstFrame = false;
		return bChanged;
	}

	void FGraphEditorPanel::DrawPropertyPanel(FGraphModel& Model)
	{
		ed::SetCurrentEditor(Context);
		ed::NodeId SelectedIds[1];
		const int Count = ed::GetSelectedNodes(SelectedIds, 1);
		ed::SetCurrentEditor(nullptr);

		if (Count == 0)
		{
			ImGui::TextDisabled("No node selected.");
			return;
		}

		FNodeRecord* Rec = Model.FindNode(SelectedIds[0].Get());
		if (!Rec)
		{
			return;
		}

		ImGui::Text("%s", Rec->Node->GetTypeName());
		ImGui::TextDisabled("Id %llu", static_cast<unsigned long long>(Rec->Id));

		// Pull the node's description from the same registry entry the palette
		// and graph hover tooltip use so the wording stays in sync.
		{
			const char* TypeName = Rec->Node->GetTypeName();
			for (const FNodeRegistration& Reg : GetNodeRegistry())
			{
				if (std::strcmp(Reg.TypeName, TypeName) == 0)
				{
					if (Reg.Description != nullptr && Reg.Description[0] != '\0')
					{
						ImGui::PushTextWrapPos(0.0f);
						ImGui::TextDisabled("%s", Reg.Description);
						ImGui::PopTextWrapPos();
					}
					break;
				}
			}
		}
		ImGui::Separator();

		const auto Infos = Rec->Node->GetParamInfos();
		if (Infos.empty())
		{
			ImGui::TextDisabled("(no parameters)");
			return;
		}

		// Param edits write the atomic directly (so the slider tracks immediately
		// and GetParamValue stays consistent for UI rendering) AND push a queued
		// SetParam so the audio thread sees the same change in the same order
		// as everything else flowing through the ring.
		const FCommandSink Sink{ CommandRing, Rec->Id };
		auto WriteParam = [&](uint32_t Index, float Value)
		{
			Rec->Node->SetParamValue(Index, Value);
			Sink.SetParam(Index, Value);
		};

		for (uint32_t I = 0; I < Infos.size(); ++I)
		{
			const FParamInfo& Info = Infos[I];
			float Value = Rec->Node->GetParamValue(I);

			switch (Info.Kind)
			{
				case EParamKind::Choice:
				{
					int Index = static_cast<int>(Value);
					if (Index < 0)
					{
						Index = 0;
					}
					if (Index >= static_cast<int>(Info.Choices.size()))
					{
						Index = static_cast<int>(Info.Choices.size()) - 1;
					}
					const char* Preview = Info.Choices.empty() ? "" : Info.Choices[Index].c_str();
					const bool bComboOpen = ImGui::BeginCombo(Info.Name.c_str(), Preview);
					// Tooltip on the combo's preview row, suppressed while the popup is open.
					if (!bComboOpen && !Info.Description.empty() && ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", Info.Description.c_str());
					}
					if (bComboOpen)
					{
						for (int C = 0; C < static_cast<int>(Info.Choices.size()); ++C)
						{
							const bool bSelected = (C == Index);
							if (ImGui::Selectable(Info.Choices[C].c_str(), bSelected))
							{
								WriteParam(I, static_cast<float>(C));
							}
							if (bSelected)
							{
								ImGui::SetItemDefaultFocus();
							}
						}
						ImGui::EndCombo();
					}
					break;
				}
				case EParamKind::Bool:
				{
					bool bChecked = Value > 0.5f;
					if (ImGui::Checkbox(Info.Name.c_str(), &bChecked))
					{
						WriteParam(I, bChecked ? 1.0f : 0.0f);
					}
					if (!Info.Description.empty() && ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", Info.Description.c_str());
					}
					break;
				}
				case EParamKind::Float:
				default:
				{
					const ImGuiSliderFlags Flags = Info.bLogarithmic ? ImGuiSliderFlags_Logarithmic : 0;
					if (ImGui::SliderFloat(Info.Name.c_str(), &Value, Info.MinValue, Info.MaxValue, "%.3f", Flags))
					{
						WriteParam(I, Value);
					}
					if (!Info.Description.empty() && ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", Info.Description.c_str());
					}
					break;
				}
			}
		}

		// Custom UI hooks for nodes that need more than the standard param widgets.
		if (auto* Adsr = dynamic_cast<FAdsr*>(Rec->Node.get()))
		{
			DrawAdsrUI(*Adsr);
		}
	}

	void FGraphEditorPanel::DrawKeyboardPanel(FGraphModel& Model)
	{
		bool bAnyDrawn = false;
		for (const auto& [Id, Rec] : Model.GetNodes())
		{
			if (auto* Kbd = dynamic_cast<FVirtualKeyboard*>(Rec.Node.get()))
			{
				if (bAnyDrawn)
				{
					ImGui::Separator();
				}
				ImGui::Text("Virtual Keyboard (Id %llu)", static_cast<unsigned long long>(Id));
				DrawVirtualKeyboardUI(*Kbd, FCommandSink{ CommandRing, Id });
				bAnyDrawn = true;
			}
		}
		if (!bAnyDrawn)
		{
			ImGui::TextDisabled("Add a Virtual Keyboard node from the graph's right-click menu.");
		}
	}
}
