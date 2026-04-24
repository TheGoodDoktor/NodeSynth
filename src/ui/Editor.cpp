#include "ui/Editor.h"

#include <algorithm>
#include <cstdio>
#include <memory>

#include <imgui.h>
#include <imgui_node_editor.h>

#include "dsp/Gain.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"

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

	FGraphEditorPanel::FGraphEditorPanel()
	{
		ed::Config EditorConfig;
		EditorConfig.SettingsFile = nullptr; // don't persist to disk yet
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
			ImGui::TextUnformatted(Node.GetTypeName());
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

			if (ImGui::MenuItem("Oscillator"))
			{
				SpawnNode(std::make_shared<FOscillator>());
			}
			if (ImGui::MenuItem("Gain"))
			{
				SpawnNode(std::make_shared<FGain>());
			}
			if (ImGui::MenuItem("Output"))
			{
				SpawnNode(std::make_shared<FOutput>());
			}
			ImGui::EndPopup();
		}
		ed::Resume();

		ed::End();
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
		ImGui::Separator();

		const auto Infos = Rec->Node->GetParamInfos();
		if (Infos.empty())
		{
			ImGui::TextDisabled("(no parameters)");
			return;
		}

		for (uint32_t I = 0; I < Infos.size(); ++I)
		{
			const FParamInfo& Info = Infos[I];
			float Value = Rec->Node->GetParamValue(I);
			const ImGuiSliderFlags Flags = Info.bLogarithmic ? ImGuiSliderFlags_Logarithmic : 0;
			if (ImGui::SliderFloat(Info.Name.c_str(), &Value, Info.MinValue, Info.MaxValue, "%.3f", Flags))
			{
				Rec->Node->SetParamValue(I, Value);
			}
		}
	}
}
