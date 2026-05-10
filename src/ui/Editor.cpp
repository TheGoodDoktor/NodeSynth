#include "ui/Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_set>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor.h>
#include <nfd.h>

#include "dsp/Adsr.h"
#include "dsp/Gain.h"
#include "dsp/GateButton.h"
#include "dsp/Meter.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/Scope.h"
#include "dsp/Sequencer.h"
#include "dsp/SidPlayer.h"
#include "dsp/Svf.h"
#include "dsp/WavetableOscillator.h"
#include "dsp/Vca.h"
#include "midi/MidiDeviceManager.h"
#include "ui/AdsrUI.h"
#include "ui/MeterUI.h"
#include "ui/NodeIcons.h"
#include "ui/NodeRegistry.h"
#include "ui/Palette.h"
#include "ui/ScopeUI.h"
#include "ui/SequencerUI.h"
#include "ui/SidPlayerUI.h"
#include "ui/WavetableUI.h"

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

	namespace
	{
		// Apply a CC value (0..127) to a node param, using the param's kind
		// (Float / Choice / Bool) and range. Mirrors the slider-widget dual
		// write: SetParamValue on the node + push SetParam audio command.
		void ApplyCcToParam(FGraphModel& Model, const FMidiMapping& M, uint8_t CcValue,
			FAudioCommandRing* CommandRing)
		{
			FNodeRecord* Rec = Model.FindNode(M.NodeId);
			if (!Rec || !Rec->Node) { return; }
			const auto Infos = Rec->Node->GetParamInfos();
			if (M.ParamIndex >= Infos.size()) { return; }
			const FParamInfo& Info = Infos[M.ParamIndex];

			const float Norm = static_cast<float>(CcValue) / 127.0f;
			float NewValue = 0.0f;
			switch (Info.Kind)
			{
				case EParamKind::Bool:
					NewValue = (CcValue >= 64) ? 1.0f : 0.0f;
					break;
				case EParamKind::Choice:
				{
					if (Info.Choices.empty()) { return; }
					const int32_t Count = static_cast<int32_t>(Info.Choices.size());
					int32_t Idx = static_cast<int32_t>(Norm * Count);
					if (Idx < 0) { Idx = 0; }
					if (Idx >= Count) { Idx = Count - 1; }
					NewValue = static_cast<float>(Idx);
					break;
				}
				case EParamKind::Float:
				default:
				{
					if (Info.bLogarithmic && Info.MinValue > 0.0f && Info.MaxValue > 0.0f)
					{
						const float Ratio = Info.MaxValue / Info.MinValue;
						NewValue = Info.MinValue * std::pow(Ratio, Norm);
					}
					else
					{
						NewValue = Info.MinValue + Norm * (Info.MaxValue - Info.MinValue);
					}
					break;
				}
				case EParamKind::String:
					return;  // not mappable
			}

			Rec->Node->SetParamValue(M.ParamIndex, NewValue);
			if (CommandRing)
			{
				CommandRing->Push(FAudioCommand::MakeSetParam(M.NodeId, M.ParamIndex, NewValue));
			}
		}

	}

	bool FGraphEditorPanel::Draw(FGraphModel& Model)
	{
		bool bChanged = false;

		// MIDI CC drain — runs every frame regardless of whether the property
		// panel is showing a node. CC events feed two consumers:
		//   1) Learn mode: capture the next CC after a 200 ms guard window.
		//   2) Otherwise: apply any matching mapping to its target param.
		if (MidiManager != nullptr)
		{
			const double Now = ImGui::GetTime();
			MidiManager->DrainCcEvents([&](uint8_t Channel, uint8_t Cc, uint8_t Value)
			{
				if (LearnTargetNodeId != 0 && (Now - LearnStartTimeSeconds) > 0.2)
				{
					FMidiMapping M;
					M.Channel = Channel;
					M.Cc = Cc;
					M.NodeId = LearnTargetNodeId;
					M.ParamIndex = LearnTargetParamIndex;
					Model.AddMidiMapping(M);
					LearnTargetNodeId = 0;
					return;
				}
				// Apply existing mappings. Channel == 0 means "any channel".
				for (const FMidiMapping& M : Model.GetMidiMappings())
				{
					if (M.Cc != Cc) { continue; }
					if (M.Channel != 0 && M.Channel != Channel) { continue; }
					ApplyCcToParam(Model, M, Value, CommandRing);
				}
			});
		}

		// Esc cancels learn mode without binding.
		if (LearnTargetNodeId != 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			LearnTargetNodeId = 0;
		}

		// Compile-error banner. Sits above the editor canvas — high visibility,
		// dismissed automatically when the user fixes the link.
		const FCompileError& CompileError = Model.GetLastCompileError();
		if (CompileError.bHasError)
		{
			const ImVec4 ErrorColour(1.0f, 0.45f, 0.40f, 1.0f);
			ImGui::PushStyleColor(ImGuiCol_Text, ErrorColour);
			ImGui::TextUnformatted("Compile error");
			ImGui::PopStyleColor();
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(CompileError.Message.c_str());
			ImGui::PopTextWrapPos();
			ImGui::TextDisabled(
				"Audio is still playing the last working patch — fix the highlighted link to apply your edits.");
			ImGui::Separator();
		}

		ed::SetCurrentEditor(Context);
		ed::Begin("Node Editor", ImVec2(0.0f, 0.0f));

		// Title-bar screen rects per node, captured during the draw loop. Used
		// after ed::End to scope the node-tooltip hover check to the title only,
		// so hovering a pin doesn't simultaneously fire the node tooltip.
		std::unordered_map<FNodeId, std::pair<ImVec2, ImVec2>> NodeTitleRects;

		// Pre-compute which pins are connected so the icon renderer can show
		// filled vs outlined glyphs without each pin walking the link list.
		// Key matches EncodePinId so lookups are O(1).
		std::unordered_set<uint64_t> ConnectedPins;
		ConnectedPins.reserve(Model.GetLinks().size() * 2);
		for (const FLink& L : Model.GetLinks())
		{
			ConnectedPins.insert(EncodePinId(L.FromNode, L.FromPort, true));
			ConnectedPins.insert(EncodePinId(L.ToNode,   L.ToPort,   false));
		}

		// Draw nodes.
		for (const auto& [Id, Rec] : Model.GetNodes())
		{
			const INode& Node = *Rec.Node;
			const auto InPorts = Node.GetInputPorts();
			const auto OutPorts = Node.GetOutputPorts();

			ed::BeginNode(ed::NodeId(Id));
			ImGui::BeginGroup();
			IconBeforeText(Node.GetTypeName(), ImGui::GetTextLineHeight());
			ImGui::TextUnformatted(Node.GetTypeName());
			if (Rec.bPerVoice)
			{
				// Per-voice badge: small bracketed label after the type name so
				// it's obvious at a glance which nodes will be cloned per voice.
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "[poly]");
			}
			ImGui::EndGroup();
			NodeTitleRects[Id] = { ImGui::GetItemRectMin(), ImGui::GetItemRectMax() };
			ImGui::Dummy(ImVec2(120.0f, 2.0f));

			const size_t Rows = std::max(InPorts.size(), OutPorts.size());
			const float PinIconSize = ImGui::GetTextLineHeight() * 0.85f;
			const float IconLabelGap = 4.0f;

			// Pre-measure each pin column so output pins can sit flush against
			// the node's right edge. Measure title width too so a wide title
			// pushes the column out instead of being clipped by the body.
			float MaxLeftWidth = 0.0f;
			float MaxRightWidth = 0.0f;
			for (size_t Row = 0; Row < Rows; ++Row)
			{
				if (Row < InPorts.size())
				{
					const float W = PinIconSize + IconLabelGap
						+ ImGui::CalcTextSize(InPorts[Row].Name.c_str()).x;
					if (W > MaxLeftWidth) { MaxLeftWidth = W; }
				}
				if (Row < OutPorts.size())
				{
					const float W = ImGui::CalcTextSize(OutPorts[Row].Name.c_str()).x
						+ IconLabelGap + PinIconSize;
					if (W > MaxRightWidth) { MaxRightWidth = W; }
				}
			}
			constexpr float MinNodeWidth = 120.0f;
			constexpr float ColumnGap = 24.0f;
			const float TitleW = ImGui::GetTextLineHeight() + 6.0f
				+ ImGui::CalcTextSize(Node.GetTypeName()).x
				+ (Rec.bPerVoice ? ImGui::CalcTextSize(" [poly]").x : 0.0f);
			const float NodeContentWidth = std::max({
				MinNodeWidth,
				TitleW,
				MaxLeftWidth + ColumnGap + MaxRightWidth
			});

			for (size_t Row = 0; Row < Rows; ++Row)
			{
				const float RowStartX = ImGui::GetCursorPosX();
				if (Row < InPorts.size())
				{
					const uint64_t Pid = EncodePinId(Id, static_cast<uint32_t>(Row), false);
					const bool bConn = ConnectedPins.count(Pid) > 0;
					ed::BeginPin(ed::PinId(Pid), ed::PinKind::Input);
					DrawPinIcon(InPorts[Row].Type, bConn, PinIconSize);
					ImGui::TextUnformatted(InPorts[Row].Name.c_str());
					ed::EndPin();
				}
				else
				{
					ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
				}

				if (Row < OutPorts.size())
				{
					// Right-align each pin individually so the pin icons line
					// up vertically on the node's right edge. Short labels
					// would otherwise leave a gap between text and the
					// (widest-pin-derived) column position.
					const float ThisPinW = ImGui::CalcTextSize(OutPorts[Row].Name.c_str()).x
						+ IconLabelGap + PinIconSize;
					const float ThisPinX = NodeContentWidth - ThisPinW;
					ImGui::SameLine();
					ImGui::SetCursorPosX(RowStartX + ThisPinX);
					const uint64_t Pid = EncodePinId(Id, static_cast<uint32_t>(Row), true);
					const bool bConn = ConnectedPins.count(Pid) > 0;
					ed::BeginPin(ed::PinId(Pid), ed::PinKind::Output);
					ImGui::TextUnformatted(OutPorts[Row].Name.c_str());
					ImGui::SameLine(0.0f, IconLabelGap);
					DrawPinIcon(OutPorts[Row].Type, bConn, PinIconSize);
					ed::EndPin();
				}
			}

			// Per-node body decorations (live readouts, etc.). Drawn between
			// the port rows and ed::EndNode so they stack inside the node box.
			if (auto* Meter = dynamic_cast<FMeter*>(Rec.Node.get()))
			{
				DrawMeterNodeBody(*Meter);
			}

			ed::EndNode();

			// Tinted header band per node category. Drawn into the node's
			// background draw list so it sits behind the title text but
			// above the editor's node fill. Uses the captured title rect,
			// expanded slightly to bleed under the node's rounded border.
			{
				const auto It = NodeTitleRects.find(Id);
				if (It != NodeTitleRects.end())
				{
					ImDrawList* BgDraw = ed::GetNodeBackgroundDrawList(ed::NodeId(Id));
					if (BgDraw != nullptr)
					{
						const ImVec2 TitleMin = It->second.first;
						const ImVec2 TitleMax = It->second.second;
						// Tint = full category colour with low alpha, so the
						// title text stays readable. Alpha 70/255 ≈ 27 %.
						const ImU32 RawCol = GetCategoryColor(Node.GetTypeName());
						const ImU32 TintedCol = (RawCol & 0x00FFFFFF) | (70u << 24);
						// Expand the rect by the node-editor's padding so the
						// fill reaches the node border on top + sides without
						// bleeding past the bottom (where ports start).
						constexpr float HeaderPad = 8.0f;
						const ImVec2 BarMin(TitleMin.x - HeaderPad, TitleMin.y - HeaderPad);
						const ImVec2 BarMax(TitleMax.x + HeaderPad, TitleMax.y + 2.0f);
						const float Rounding = ed::GetStyle().NodeRounding;
						BgDraw->AddRectFilled(BarMin, BarMax, TintedCol, Rounding,
							ImDrawFlags_RoundCornersTop);
					}
				}
			}

			if (bFirstFrame)
			{
				ed::SetNodePosition(ed::NodeId(Id), ImVec2(Rec.PositionX, Rec.PositionY));
			}
		}

		// Draw links. Color-code by source-port type to match the pin icons
		// (audio = warm orange, control = teal) so routing is readable at a
		// glance. The link the last compile rejected renders in red. All
		// links use a fatter stroke than imgui-node-editor's default —
		// thin hairlines are hard to follow on dense patches.
		const ImColor LinkAudio  (255, 170,  60, 255);
		const ImColor LinkControl(80,  220, 200, 255);
		const ImColor LinkBad    (255,  90,  90, 255);
		constexpr float LinkThickness = 3.0f;
		for (const FLink& L : Model.GetLinks())
		{
			const bool bIsBadLink = CompileError.bHasError
				&& L.FromNode == CompileError.FromNode
				&& L.FromPort == CompileError.FromPort
				&& L.ToNode == CompileError.ToNode
				&& L.ToPort == CompileError.ToPort;

			ImColor LinkCol = LinkAudio;
			if (!bIsBadLink)
			{
				if (FNodeRecord* Rec = Model.FindNode(L.FromNode); Rec && Rec->Node)
				{
					const auto Ports = Rec->Node->GetOutputPorts();
					if (L.FromPort < Ports.size() && Ports[L.FromPort].Type == EPortType::Control)
					{
						LinkCol = LinkControl;
					}
				}
			}
			else
			{
				LinkCol = LinkBad;
			}

			ed::Link(ed::LinkId(L.Id),
				ed::PinId(EncodePinId(L.FromNode, L.FromPort, true)),
				ed::PinId(EncodePinId(L.ToNode, L.ToPort, false)),
				LinkCol, LinkThickness);
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
						// Pre-flight the polyphony rule so the user gets a red
						// tooltip mid-drag instead of a silent compile failure
						// after dropping. Other failures (cycle, port type
						// mismatch) are still caught by AddLink itself.
						const std::string PolyReason =
							Model.ValidateLinkPolyphony(FromNode, FromPort, ToNode);
						if (!PolyReason.empty())
						{
							ed::Suspend();
							ImGui::SetTooltip("%s", PolyReason.c_str());
							ed::Resume();
							ed::RejectNewItem(ImColor(255, 90, 90), 2.0f);
						}
						else if (ed::AcceptNewItem())
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

		// Deletion: user presses Delete with links/nodes selected. Wrap the
		// whole batch in a composite history entry so a multi-select Delete is
		// a single undoable action.
		if (ed::BeginDelete())
		{
			const bool bHasHistory = History != nullptr;
			if (bHasHistory) { History->BeginComposite(); }
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
			if (bHasHistory) { History->EndComposite(); }
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

				ImGui::Separator();

				if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, bCloneable))
				{
					if (auto Clone = Rec->Node->Clone())
					{
						const FNodeId NewId = Model.AddNode(Clone,
							Rec->PositionX + 40.0f, Rec->PositionY + 40.0f);
						if (NewId != 0)
						{
							ed::SetNodePosition(ed::NodeId(NewId),
								ImVec2(Rec->PositionX + 40.0f, Rec->PositionY + 40.0f));
							bChanged = true;
						}
					}
				}
				if (ImGui::MenuItem("Delete", "Del"))
				{
					Model.RemoveNode(NodeContextTarget);
					bChanged = true;
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

		// Hover tooltips. Pin tooltip wins if a pin is under the cursor; node
		// tooltip fires only when the cursor is inside the captured title rect.
		// Both suspend out of the editor canvas so they render in screen space,
		// and both are suppressed during drag-drop so they don't fight previews.
		const ed::PinId HoveredPin = ed::GetHoveredPin();
		const bool bDragActive = ImGui::GetDragDropPayload() != nullptr;
		if (HoveredPin && !bDragActive)
		{
			FNodeId NodeId = 0;
			uint32_t PortIndex = 0;
			bool bIsOutput = false;
			DecodePinId(HoveredPin.Get(), NodeId, PortIndex, bIsOutput);
			if (FNodeRecord* Rec = Model.FindNode(NodeId))
			{
				const auto Ports = bIsOutput
					? Rec->Node->GetOutputPorts()
					: Rec->Node->GetInputPorts();
				if (PortIndex < Ports.size())
				{
					const FPortInfo& Port = Ports[PortIndex];
					if (!Port.Description.empty())
					{
						ed::Suspend();
						ImGui::BeginTooltip();
						ImGui::Text("%s  (%s %s)",
							Port.Name.c_str(),
							Port.Type == EPortType::Audio ? "Audio" : "Control",
							bIsOutput ? "out" : "in");
						ImGui::Separator();
						ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
						ImGui::TextUnformatted(Port.Description.c_str());
						ImGui::PopTextWrapPos();
						ImGui::EndTooltip();
						ed::Resume();
					}
				}
			}
		}
		else if (!HoveredPin && !bDragActive)
		{
			if (const ed::NodeId HoveredNode = ed::GetHoveredNode())
			{
				const FNodeId Id = HoveredNode.Get();
				auto RectIt = NodeTitleRects.find(Id);
				if (RectIt != NodeTitleRects.end())
				{
					const ImVec2 Mouse = ImGui::GetMousePos();
					const ImVec2& Min = RectIt->second.first;
					const ImVec2& Max = RectIt->second.second;
					const bool bInTitle =
						Mouse.x >= Min.x && Mouse.x < Max.x &&
						Mouse.y >= Min.y && Mouse.y < Max.y;
					if (bInTitle)
					{
						if (FNodeRecord* Rec = Model.FindNode(Id))
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
				}
			}
		}

		// Position undo/redo. Detect drag start by per-node position change
		// while the mouse is held; finalise (push history entry + sync model)
		// only when the mouse is actually released. This way a momentary
		// stillness mid-drag (mouse paused while still held) doesn't look like
		// a drop and produce a stray history entry. Multi-select drags are
		// wrapped in a composite group so the whole batch is one undo.
		if (!bFirstFrame)
		{
			const bool bMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
			constexpr float DragEpsilon = 0.5f;

			// Pass 1: track per-node movement; mark `bDragging = true` for any
			// node that has moved at any point while the mouse has been down.
			for (const auto& [Id, Rec] : Model.GetNodes())
			{
				const ImVec2 Cur = ed::GetNodePosition(ed::NodeId(Id));
				FNodeDragState& State = NodeDragStates[Id];
				if (!State.bSeen)
				{
					State.LastX = Cur.x;
					State.LastY = Cur.y;
					State.bSeen = true;
					continue;
				}
				const float DX = Cur.x - State.LastX;
				const float DY = Cur.y - State.LastY;
				const bool bMovedThisFrame = (std::fabs(DX) > DragEpsilon || std::fabs(DY) > DragEpsilon);

				if (bMovedThisFrame && !State.bDragging)
				{
					// Drag is starting on this node. Capture position at the
					// start of the previous frame (i.e. before the move that
					// just happened).
					State.DragStartX = State.LastX;
					State.DragStartY = State.LastY;
					State.bDragging = true;
				}

				// Always advance LastFramePos so the next frame sees the latest
				// position as its baseline.
				State.LastX = Cur.x;
				State.LastY = Cur.y;
			}

			// Pass 2: if the mouse was just released, finalise any in-progress
			// drags as a single (composite if needed) history entry.
			if (!bMouseDown)
			{
				size_t NumDropped = 0;
				for (auto& [Id, State] : NodeDragStates)
				{
					if (State.bDragging) { ++NumDropped; }
				}
				if (NumDropped > 0)
				{
					const bool bComposite = NumDropped > 1 && History != nullptr;
					if (bComposite) { History->BeginComposite(); }
					for (auto& [Id, State] : NodeDragStates)
					{
						if (!State.bDragging) { continue; }
						const float NewX = State.LastX;
						const float NewY = State.LastY;
						const bool bActuallyMoved =
							std::fabs(NewX - State.DragStartX) > DragEpsilon ||
							std::fabs(NewY - State.DragStartY) > DragEpsilon;
						if (bActuallyMoved && History != nullptr)
						{
							FEditCommand Cmd;
							Cmd.Type = EEditCommand::SetNodePosition;
							Cmd.NodeId = Id;
							Cmd.OldX = State.DragStartX;
							Cmd.OldY = State.DragStartY;
							Cmd.NewX = NewX;
							Cmd.NewY = NewY;
							History->Push(std::move(Cmd));
						}
						if (FNodeRecord* RecMut = Model.FindNode(Id))
						{
							RecMut->PositionX = NewX;
							RecMut->PositionY = NewY;
						}
						State.bDragging = false;
					}
					if (bComposite) { History->EndComposite(); }
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
						ImGui::TextWrapped("%s", Reg.Description);
					}
					break;
				}
			}
		}
		ImGui::Separator();

		const auto Infos = Rec->Node->GetParamInfos();
		// Don't early-return here even if Infos is empty — passive-tap nodes
		// (Meter, etc.) have no params but still want their custom UI to draw.
		const bool bAllHidden = std::all_of(Infos.begin(), Infos.end(),
			[](const FParamInfo& I) { return I.bHidden; });
		if (Infos.empty() || bAllHidden)
		{
			ImGui::TextDisabled("(no parameters)");
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

		// Push a single SetParam edit-history entry with both old + new values.
		// Used by Bool/Choice (instantaneous) and Float (after slider release).
		auto PushSetParamEdit = [&](uint32_t Index, float OldVal, float NewVal)
		{
			if (History == nullptr || OldVal == NewVal) { return; }
			FEditCommand Cmd;
			Cmd.Type = EEditCommand::SetParam;
			Cmd.NodeId = Rec->Id;
			Cmd.ParamIndex = Index;
			Cmd.OldValue = OldVal;
			Cmd.NewValue = NewVal;
			History->Push(std::move(Cmd));
		};

		for (uint32_t I = 0; I < Infos.size(); ++I)
		{
			const FParamInfo& Info = Infos[I];
			if (Info.bHidden)
			{
				continue;  // hidden params (e.g. sequencer grid cells) are
				           // surfaced via custom UI hooks below.
			}
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
								const float Before = Value;
								const float After = static_cast<float>(C);
								WriteParam(I, After);
								PushSetParamEdit(I, Before, After);
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
					const float Before = Value;
					bool bChecked = Value > 0.5f;
					if (ImGui::Checkbox(Info.Name.c_str(), &bChecked))
					{
						const float After = bChecked ? 1.0f : 0.0f;
						WriteParam(I, After);
						PushSetParamEdit(I, Before, After);
					}
					if (!Info.Description.empty() && ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", Info.Description.c_str());
					}
					break;
				}
				case EParamKind::String:
				{
					std::string Current = Rec->Node->GetParamString(I);
					// Editable text field; commits the new path on Enter or
					// when the user clicks elsewhere (EnterReturnsTrue).
					char Buf[1024];
					const size_t Copy = std::min(Current.size(), sizeof(Buf) - 1);
					std::memcpy(Buf, Current.data(), Copy);
					Buf[Copy] = '\0';
					ImGui::PushID(static_cast<int>(I));
					ImGui::SetNextItemWidth(-FLT_MIN - 40.0f);
					if (ImGui::InputText(Info.Name.c_str(), Buf, sizeof(Buf),
						ImGuiInputTextFlags_EnterReturnsTrue))
					{
						Rec->Node->SetParamString(I, Buf);
					}
					if (!Info.Description.empty() && ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", Info.Description.c_str());
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("..."))
					{
						nfdu8char_t* OutPath = nullptr;
						// Filter dispatch by node type — each node that exposes
						// a String file-picker param gets its own extension(s).
						const char* TypeName = Rec->Node->GetTypeName();
						nfdu8filteritem_t Filter[1] = { { "SID Tune", "sid" } };
						if (TypeName != nullptr
							&& std::strcmp(TypeName, "WavetableOscillator") == 0)
						{
							Filter[0] = { "Wavetable WAV", "wav" };
						}
						nfdopendialogu8args_t Args = {};
						Args.filterList = Filter;
						Args.filterCount = 1;
						const nfdresult_t Result = NFD_OpenDialogU8_With(&OutPath, &Args);
						if (Result == NFD_OKAY && OutPath)
						{
							Rec->Node->SetParamString(I, OutPath);
							NFD_FreePathU8(OutPath);
						}
					}
					ImGui::PopID();
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
					// Slider drag coalescing: capture the value at the moment
					// the widget was first activated, push one history entry
					// when the user releases. ImGui's IsItemActivated /
					// IsItemDeactivatedAfterEdit are designed for exactly this.
					if (ImGui::IsItemActivated())
					{
						ActiveParamNode = Rec->Id;
						ActiveParamIndex = I;
						ActiveParamOldValue = Rec->Node->GetParamValue(I);
						bActiveParamCaptured = true;
					}
					if (ImGui::IsItemDeactivatedAfterEdit() && bActiveParamCaptured
						&& ActiveParamNode == Rec->Id && ActiveParamIndex == I)
					{
						PushSetParamEdit(I, ActiveParamOldValue, Rec->Node->GetParamValue(I));
						bActiveParamCaptured = false;
					}
					if (!Info.Description.empty() && ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", Info.Description.c_str());
					}
					break;
				}
			}

			// MIDI Learn UI per param: right-click context menu (Learn / Unmap)
			// + small inline badge showing the bound CC or learn-mode marker.
			// Attached to whichever widget the switch last drew. String params
			// are not MIDI-mappable; their context menu is silently skipped.
			if (Info.Kind != EParamKind::String)
			{
				const FMidiMapping* Mapped = Model.FindMidiMapping(Rec->Id, I);
				const bool bLearningThis = (LearnTargetNodeId == Rec->Id && LearnTargetParamIndex == I);

				ImGui::PushID(static_cast<int>(I));
				if (ImGui::BeginPopupContextItem("midi_ctx"))
				{
					if (ImGui::MenuItem("MIDI Learn"))
					{
						LearnTargetNodeId = Rec->Id;
						LearnTargetParamIndex = I;
						LearnStartTimeSeconds = ImGui::GetTime();
					}
					if (ImGui::MenuItem("Unmap MIDI", nullptr, false, Mapped != nullptr))
					{
						if (Mapped)
						{
							Model.RemoveMidiMapping(Mapped->Channel, Mapped->Cc);
						}
					}
					ImGui::EndPopup();
				}
				if (bLearningThis)
				{
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f), "[LEARN]");
				}
				else if (Mapped)
				{
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
						"CC%d", static_cast<int>(Mapped->Cc));
				}
				ImGui::PopID();
			}
		}

		// Custom UI hooks for nodes that need more than the standard param widgets.
		if (auto* Adsr = dynamic_cast<FAdsr*>(Rec->Node.get()))
		{
			DrawAdsrUI(*Adsr);
		}
		if (auto* Seq = dynamic_cast<FSequencer*>(Rec->Node.get()))
		{
			DrawSequencerUI(*Seq, Sink);
		}
		if (auto* Scope = dynamic_cast<FScope*>(Rec->Node.get()))
		{
			DrawScopeUI(*Scope);
		}
		if (auto* Meter = dynamic_cast<FMeter*>(Rec->Node.get()))
		{
			DrawMeterUI(*Meter);
		}
		if (auto* Sid = dynamic_cast<FSidPlayer*>(Rec->Node.get()))
		{
			DrawSidPlayerUI(*Sid);
		}
		if (auto* Wt = dynamic_cast<FWavetableOscillator*>(Rec->Node.get()))
		{
			DrawWavetableUI(*Wt);
		}
	}

	void FGraphEditorPanel::DrawKeyboardPanel(FGraphModel& /*Model*/)
	{
		// Project-level note input: a MIDI device combo + the on-screen
		// keyboard. Both feed the audio command queue (notes + velocity);
		// the audio thread broadcasts to every voice allocator.
		if (MidiManager != nullptr)
		{
			const auto Names = MidiManager->GetDeviceNames();
			const int32_t Selected = MidiManager->GetSelectedPort();
			const char* Preview = (Selected >= 0 && Selected < static_cast<int32_t>(Names.size()))
				? Names[Selected].c_str()
				: "(none)";
			ImGui::SetNextItemWidth(220.0f);
			if (ImGui::BeginCombo("MIDI Device", Preview))
			{
				if (ImGui::Selectable("(none)", Selected < 0))
				{
					MidiManager->SetDevicePort(-1);
				}
				for (int32_t I = 0; I < static_cast<int32_t>(Names.size()); ++I)
				{
					const bool bSel = (I == Selected);
					if (ImGui::Selectable(Names[I].c_str(), bSel))
					{
						MidiManager->SetDevicePort(I);
					}
					if (bSel) { ImGui::SetItemDefaultFocus(); }
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			int32_t Channel = MidiManager->GetChannelFilter();
			ImGui::SetNextItemWidth(80.0f);
			static const char* ChannelLabels[17] = {
				"Omni", "Ch 1", "Ch 2", "Ch 3", "Ch 4", "Ch 5", "Ch 6", "Ch 7",
				"Ch 8", "Ch 9", "Ch 10", "Ch 11", "Ch 12", "Ch 13", "Ch 14", "Ch 15", "Ch 16"
			};
			if (ImGui::Combo("##ch", &Channel, ChannelLabels, 17))
			{
				MidiManager->SetChannelFilter(Channel);
			}
		}
		ImGui::Separator();
		Keyboard.Draw(FCommandSink{ CommandRing, 0 });
	}

	void FGraphEditorPanel::DrawPatchInfoPanel(FGraphModel& Model)
	{
		FPatchMetadata& Meta = Model.GetMetadata();

		// Persistent backing buffers for ImGui InputText. Sized generously;
		// the values are sync'd to / from the metadata each frame.
		static char NameBuf[128] = {};
		static char AuthorBuf[128] = {};
		static char NotesBuf[1024] = {};

		auto SyncString = [](char* Buf, size_t BufSize, const std::string& Src)
		{
			const size_t Copy = std::min(Src.size(), BufSize - 1);
			std::memcpy(Buf, Src.data(), Copy);
			Buf[Copy] = '\0';
		};

		// Detect external edits (e.g. patch load) by comparing buffer to model.
		// Cheap because strings are short.
		if (std::string(NameBuf) != Meta.Name) { SyncString(NameBuf, sizeof(NameBuf), Meta.Name); }
		if (std::string(AuthorBuf) != Meta.Author) { SyncString(AuthorBuf, sizeof(AuthorBuf), Meta.Author); }
		if (std::string(NotesBuf) != Meta.Notes) { SyncString(NotesBuf, sizeof(NotesBuf), Meta.Notes); }

		if (ImGui::InputText("Name", NameBuf, sizeof(NameBuf)))
		{
			Meta.Name = NameBuf;
		}
		if (ImGui::InputText("Author", AuthorBuf, sizeof(AuthorBuf)))
		{
			Meta.Author = AuthorBuf;
		}
		ImGui::DragFloat("BPM", &Meta.Bpm, 0.5f, 1.0f, 400.0f, "%.1f");
		ImGui::TextDisabled("Notes");
		if (ImGui::InputTextMultiline("##notes", NotesBuf, sizeof(NotesBuf),
			ImVec2(-1.0f, ImGui::GetTextLineHeight() * 6.0f)))
		{
			Meta.Notes = NotesBuf;
		}

		ImGui::Separator();
		if (Meta.SampleRateHint > 0.0)
		{
			ImGui::TextDisabled("Saved at %.0f Hz", Meta.SampleRateHint);
		}
		else
		{
			ImGui::TextDisabled("Sample rate will be recorded on next save.");
		}
	}

	void FGraphEditorPanel::DrawHistoryPanel(FGraphModel& Model)
	{
		(void)Model;
		if (History == nullptr)
		{
			ImGui::TextDisabled("(no history available)");
			return;
		}
		if (!History->CanUndo() && !History->CanRedo())
		{
			ImGui::TextDisabled("No edits yet.");
			return;
		}

		const size_t NumRedo = History->RedoSize();
		const size_t NumUndo = History->UndoSize();

		// Redo entries: top of the panel. Click to redo (I+1) times so that
		// entry lands on the undo stack.
		for (size_t I = NumRedo; I-- > 0; )
		{
			const std::string Label = History->GetRedoLabel(I);
			ImGui::PushID(static_cast<int>(I + 1024));
			ImGui::TextDisabled(" v ");
			ImGui::SameLine();
			if (ImGui::SmallButton(Label.c_str()))
			{
				PendingHistoryJump = -static_cast<int32_t>(I + 1);
			}
			ImGui::PopID();
		}

		ImGui::Separator();
		ImGui::TextDisabled("— now —");
		ImGui::Separator();

		// Undo entries: most-recent first. Click to undo (I+1) times.
		for (size_t I = 0; I < NumUndo; ++I)
		{
			const std::string Label = History->GetUndoLabel(I);
			ImGui::PushID(static_cast<int>(I));
			if (ImGui::SmallButton(Label.c_str()))
			{
				PendingHistoryJump = static_cast<int32_t>(I + 1);
			}
			ImGui::PopID();
		}
	}

	void FGraphEditorPanel::DrawMidiMappingsPanel(FGraphModel& Model)
	{
		const auto& Mappings = Model.GetMidiMappings();
		if (Mappings.empty())
		{
			ImGui::TextDisabled("No MIDI mappings.");
			ImGui::TextDisabled("Right-click any param → MIDI Learn.");
			return;
		}

		// Snapshot the (channel, cc) keys before iterating so removing inside
		// the loop doesn't invalidate the underlying vector mid-traversal.
		std::vector<std::pair<uint8_t, uint8_t>> ToRemove;
		for (size_t I = 0; I < Mappings.size(); ++I)
		{
			const FMidiMapping& M = Mappings[I];
			ImGui::PushID(static_cast<int>(I));

			std::string Target = "(deleted)";
			std::string ParamName;
			if (FNodeRecord* Rec = Model.FindNode(M.NodeId); Rec && Rec->Node)
			{
				Target = Rec->Node->GetTypeName();
				const auto Infos = Rec->Node->GetParamInfos();
				if (M.ParamIndex < Infos.size())
				{
					ParamName = Infos[M.ParamIndex].Name;
				}
			}

			if (ImGui::SmallButton("x"))
			{
				ToRemove.emplace_back(M.Channel, M.Cc);
			}
			ImGui::SameLine();
			if (M.Channel == 0)
			{
				ImGui::Text("Any  CC%-3d  -> %s.%s",
					static_cast<int>(M.Cc), Target.c_str(), ParamName.c_str());
			}
			else
			{
				ImGui::Text("Ch%-2d CC%-3d  -> %s.%s",
					static_cast<int>(M.Channel), static_cast<int>(M.Cc),
					Target.c_str(), ParamName.c_str());
			}
			ImGui::PopID();
		}
		for (const auto& [Ch, Cc] : ToRemove)
		{
			Model.RemoveMidiMapping(Ch, Cc);
		}
	}
}
