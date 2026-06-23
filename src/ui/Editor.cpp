#include "ui/Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor.h>
#include <nfd.h>

#include "dsp/Adsr.h"
#include "dsp/Gain.h"
#include "dsp/GateButton.h"
#include "dsp/Subgraph.h"
#include "dsp/internal/SubgraphBoundary.h"
#include "graph/SubgraphOps.h"
#include "dsp/Meter.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/Scope.h"
#include "dsp/Sequencer.h"
#include "dsp/MicInput.h"
#include "dsp/MidiCC.h"
#include "dsp/ModulationMatrix.h"
#include "dsp/SidPlayer.h"
#include "dsp/Svf.h"
#include "dsp/WavetableOscillator.h"
#include "dsp/Vca.h"
#include "io/SubgraphBrowser.h"
#include "io/SubgraphSerializer.h"
#include "midi/MidiDeviceManager.h"
#include "ui/AdsrUI.h"
#include "ui/MeterUI.h"
#include "ui/MicInputUI.h"
#include "ui/NodeIcons.h"
#include "ui/NodeRegistry.h"
#include "ui/Palette.h"
#include "ui/ScopeUI.h"
#include "ui/MidiCCUI.h"
#include "ui/ModMatrixUI.h"
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

		bool IsSubgraphBoundary(const INode& Node)
		{
			const std::string T = Node.GetTypeName();
			return T == "_SubgraphInputs" || T == "_SubgraphOutputs";
		}

		// Node types that may not be created inside a subgraph (the patch sink
		// and the voice allocator). Mirrors FGraphModel::Compile's expansion
		// rejection so the user can't drop one in to begin with.
		bool IsForbiddenInSubgraph(const char* TypeName)
		{
			return std::strcmp(TypeName, "Output") == 0
				|| std::strcmp(TypeName, "VoiceAllocator") == 0;
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
		ed::SetCurrentEditor(nullptr);
		for (auto& Level : SubgraphStack)
		{
			if (Level && Level->Context)
			{
				ed::DestroyEditor(Level->Context);
			}
		}
		SubgraphStack.clear();
		if (Context)
		{
			ed::DestroyEditor(Context);
			Context = nullptr;
		}
	}

	void FGraphEditorPanel::OnModelReplaced()
	{
		PopToLevel(0);  // a freshly loaded patch has different subgraph defs
		bFirstFrame = true;
		NodeDragStates.clear();
	}

	void FGraphEditorPanel::EnterSubgraph(const std::shared_ptr<FSubgraphDefinition>& Def)
	{
		if (!Def)
		{
			return;
		}
		// Make sure the boundary nodes carry the definition's current pins so
		// they render (and validate links) with the right ports.
		SyncSubgraphBoundaries(*Def);

		auto Level = std::make_unique<FSubgraphLevel>();
		ed::Config Cfg;
		// No settings file: subgraph node positions live in the definition's
		// node records (and serialize with it), so the library doesn't need to
		// persist a separate canvas file per level.
		Cfg.SettingsFile = nullptr;
		Level->Context = ed::CreateEditor(&Cfg);
		Level->Model = &Def->InternalGraph;
		Level->Definition = Def;
		Level->Name = Def->Name;
		SubgraphStack.push_back(std::move(Level));

		bFirstFrame = true;        // reseed positions from the level's model
		NodeDragStates.clear();
	}

	void FGraphEditorPanel::PopToLevel(int32_t KeepDepth)
	{
		if (KeepDepth < 0)
		{
			KeepDepth = 0;
		}
		ed::SetCurrentEditor(nullptr);
		while (static_cast<int32_t>(SubgraphStack.size()) > KeepDepth)
		{
			if (SubgraphStack.back() && SubgraphStack.back()->Context)
			{
				ed::DestroyEditor(SubgraphStack.back()->Context);
			}
			SubgraphStack.pop_back();
		}
		bFirstFrame = true;
		NodeDragStates.clear();
	}

	bool FGraphEditorPanel::MakeSubgraphFromSelection(FGraphModel& PatchModel)
	{
		// Read the current canvas selection (base context).
		ed::SetCurrentEditor(Context);
		ed::NodeId SelBuf[64];
		const int Count = ed::GetSelectedNodes(SelBuf, 64);
		ed::SetCurrentEditor(nullptr);
		if (Count <= 0)
		{
			return false;
		}

		std::vector<FNodeId> Selected;
		Selected.reserve(static_cast<size_t>(Count));
		for (int I = 0; I < Count; ++I)
		{
			Selected.push_back(SelBuf[I].Get());
		}

		// The grouping is multi-step and the new instance's definition binding
		// can't be replayed by undo in v1, so don't record it in history.
		const bool bWasRecording = PatchModel.IsRecordingHistory();
		PatchModel.SetRecordHistory(false);
		const FNodeId InstId = GroupNodesIntoSubgraph(PatchModel, Selected);
		PatchModel.SetRecordHistory(bWasRecording);

		if (InstId == 0)
		{
			return false;
		}
		// Reseed canvas positions next frame so the new instance lands at the
		// selection centroid (set via the model) without an explicit
		// SetNodePosition (which needs the canvas open).
		bFirstFrame = true;
		NodeDragStates.clear();
		return true;
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

	bool FGraphEditorPanel::Draw(FGraphModel& PatchModel)
	{
		// Fold in any param edit made inside a subgraph last frame so the patch
		// recompiles and the edited definition takes audible effect.
		bool bChanged = bSubgraphParamDirty;
		bSubgraphParamDirty = false;

		// Resolve the active editing level: the patch model at the base, or the
		// deepest dived subgraph's internal graph. The rest of Draw operates on
		// `Model` / `ActiveCtx` / `ActiveHistory` so it works at any level.
		const bool bAtBase = SubgraphStack.empty();
		FGraphModel& Model = bAtBase ? PatchModel : *SubgraphStack.back()->Model;
		ed::EditorContext* const ActiveCtx = bAtBase ? this->Context : SubgraphStack.back()->Context;
		// Undo/redo for subgraph internals is deferred (PLAN-SUBGRAPHS §1.12);
		// disable history recording while inside a subgraph so its edits don't
		// push mismatched entries onto the patch's history stack.
		FEditHistory* const ActiveHistory = bAtBase ? this->History : nullptr;

		// MIDI CC drain — base level only. Learn targets and mappings reference
		// patch node ids, so they pause while editing a subgraph's internals.
		//   1) Learn mode: capture the next CC after a 200 ms guard window.
		//   2) Otherwise: apply any matching mapping to its target param.
		if (MidiManager != nullptr && bAtBase)
		{
			const double Now = ImGui::GetTime();
			MidiManager->DrainCcEvents([&](uint8_t Channel, uint8_t Cc, uint8_t Value)
			{
				if (LearnTargetNodeId != 0 && (Now - LearnStartTimeSeconds) > 0.2)
				{
					// Two flavours of learn share the LearnTarget* fields.
					// ParamIndex == LearnSentinel_MidiCcNode means the
					// learn target is an FMidiCC node — set its Cc and
					// Channel params instead of adding a graph mapping.
					if (LearnTargetParamIndex == LearnSentinel_MidiCcNode)
					{
						FNodeRecord* Rec = Model.FindNode(LearnTargetNodeId);
						if (Rec && Rec->Node)
						{
							if (auto* CcNode = dynamic_cast<FMidiCC*>(Rec->Node.get()))
							{
								CcNode->SetParamValue(FMidiCC::Param_Cc,
									static_cast<float>(Cc));
								CcNode->SetParamValue(FMidiCC::Param_Channel,
									static_cast<float>(Channel));
								if (CommandRing)
								{
									CommandRing->Push(FAudioCommand::MakeSetParam(
										LearnTargetNodeId, FMidiCC::Param_Cc,
										static_cast<float>(Cc)));
									CommandRing->Push(FAudioCommand::MakeSetParam(
										LearnTargetNodeId, FMidiCC::Param_Channel,
										static_cast<float>(Channel)));
								}
							}
						}
						LearnTargetNodeId = 0;
						return;
					}
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

		// Esc pops one subgraph level (when not typing and not cancelling learn).
		if (!bAtBase && LearnTargetNodeId == 0
			&& !ImGui::GetIO().WantTextInput
			&& ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			PendingPopTo = static_cast<int32_t>(SubgraphStack.size()) - 1;
		}

		// Ctrl+G wraps the current selection into a subgraph (patch level only).
		if (bAtBase && ImGui::GetIO().KeyCtrl
			&& !ImGui::GetIO().WantTextInput
			&& ImGui::IsKeyPressed(ImGuiKey_G, false))
		{
			if (MakeSubgraphFromSelection(Model))
			{
				bChanged = true;
			}
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

		// Breadcrumb bar — shown only when dived into one or more subgraphs.
		// Click an ancestor crumb to pop back to that level; the current level
		// (last crumb) is plain text. Pops are deferred to the end of Draw so
		// the stack isn't mutated mid-frame.
		if (!SubgraphStack.empty())
		{
			if (ImGui::SmallButton("Patch"))
			{
				PendingPopTo = 0;
			}
			for (size_t I = 0; I < SubgraphStack.size(); ++I)
			{
				ImGui::SameLine();
				ImGui::TextUnformatted(">");
				ImGui::SameLine();
				const bool bLast = (I + 1 == SubgraphStack.size());
				// Read the name live from the definition so it tracks renames.
				const std::string& LevelName = SubgraphStack[I]->Definition
					? SubgraphStack[I]->Definition->Name
					: SubgraphStack[I]->Name;
				const std::string Label = "Subgraph: " + LevelName;
				if (bLast)
				{
					ImGui::TextUnformatted(Label.c_str());
				}
				else
				{
					const std::string Id = Label + "##crumb" + std::to_string(I);
					if (ImGui::SmallButton(Id.c_str()))
					{
						PendingPopTo = static_cast<int32_t>(I + 1);
					}
				}
			}
			ImGui::Separator();
		}

		ed::SetCurrentEditor(ActiveCtx);
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

			// Subgraph instances show their definition name as the title (the
			// icon stays keyed on the "Subgraph" type name).
			const char* TitleText = Node.GetTypeName();
			if (auto* Sub = dynamic_cast<const FSubgraph*>(&Node))
			{
				if (Sub->GetDefinition() && !Sub->GetDefinition()->Name.empty())
				{
					TitleText = Sub->GetDefinition()->Name.c_str();
				}
			}

			ed::BeginNode(ed::NodeId(Id));
			ImGui::BeginGroup();
			IconBeforeText(Node.GetTypeName(), ImGui::GetTextLineHeight());
			ImGui::TextUnformatted(TitleText);
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
			const bool bHasHistory = ActiveHistory != nullptr;
			if (bHasHistory) { ActiveHistory->BeginComposite(); }
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
				// Boundary nodes define the subgraph's signature and can't be
				// deleted from the canvas (pins are managed via the pin panel).
				if (FNodeRecord* Rec = Model.FindNode(DeletedNode.Get());
					Rec && Rec->Node && IsSubgraphBoundary(*Rec->Node))
				{
					ed::RejectDeletedItem();
					continue;
				}
				if (ed::AcceptDeletedItem())
				{
					Model.RemoveNode(DeletedNode.Get());
					bChanged = true;
				}
			}
			if (bHasHistory) { ActiveHistory->EndComposite(); }
		}
		ed::EndDelete();

		// Double-click a subgraph instance to dive into its internal graph.
		// Deferred (PendingDive) so the context swap happens after the canvas
		// closes this frame.
		if (const ed::NodeId DoubleClicked = ed::GetDoubleClickedNode())
		{
			if (FNodeRecord* Rec = Model.FindNode(DoubleClicked.Get()))
			{
				if (auto* Sub = dynamic_cast<FSubgraph*>(Rec->Node.get()))
				{
					if (Sub->GetDefinition())
					{
						PendingDive = Sub->GetDefinition();
					}
				}
			}
		}

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
				const bool bBoundary = Rec->Node && IsSubgraphBoundary(*Rec->Node);
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
				if (ImGui::MenuItem("Delete", "Del", false, !bBoundary))
				{
					Model.RemoveNode(NodeContextTarget);
					bChanged = true;
				}
				if (bBoundary)
				{
					ImGui::TextDisabled("(boundary node — manage via pins)");
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

			// New subgraph: create a fresh identity-passthrough definition and
			// drop an instance. Double-click it to dive in and build its guts.
			// Only offered at the patch level for now (nesting via this menu
			// would need the parent's definition map — SG.5).
			if (bAtBase)
			{
				if (ImGui::MenuItem("New Subgraph"))
				{
					// Pick a name not already used by a definition in this patch,
					// register it in the patch's definition map (so it serializes
					// and is shared by every instance), and drop an instance.
					std::string Name;
					do
					{
						Name = "Subgraph " + std::to_string(NextSubgraphSerial++);
					}
					while (Model.FindSubgraphDefinition(Name));
					auto Def = Model.AddSubgraphDefinition(MakeEmptySubgraphDefinition(Name));
					auto Instance = std::make_shared<FSubgraph>();
					Instance->SetDefinition(std::move(Def));
					SpawnNode(std::move(Instance));
				}
				ImGui::Separator();
			}

			for (const FNodeRegistration& Reg : GetNodeRegistry())
			{
				if (!bAtBase && IsForbiddenInSubgraph(Reg.TypeName))
				{
					continue;  // can't put a sink / allocator inside a subgraph
				}
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
					const bool bComposite = NumDropped > 1 && ActiveHistory != nullptr;
					if (bComposite) { ActiveHistory->BeginComposite(); }
					for (auto& [Id, State] : NodeDragStates)
					{
						if (!State.bDragging) { continue; }
						const float NewX = State.LastX;
						const float NewY = State.LastY;
						const bool bActuallyMoved =
							std::fabs(NewX - State.DragStartX) > DragEpsilon ||
							std::fabs(NewY - State.DragStartY) > DragEpsilon;
						if (bActuallyMoved && ActiveHistory != nullptr)
						{
							FEditCommand Cmd;
							Cmd.Type = EEditCommand::SetNodePosition;
							Cmd.NodeId = Id;
							Cmd.OldX = State.DragStartX;
							Cmd.OldY = State.DragStartY;
							Cmd.NewX = NewX;
							Cmd.NewY = NewY;
							ActiveHistory->Push(std::move(Cmd));
						}
						if (FNodeRecord* RecMut = Model.FindNode(Id))
						{
							RecMut->PositionX = NewX;
							RecMut->PositionY = NewY;
						}
						State.bDragging = false;
					}
					if (bComposite) { ActiveHistory->EndComposite(); }
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
				if (Index >= 0 && Index < static_cast<int32_t>(Registry.size())
					&& (bAtBase || !IsForbiddenInSubgraph(Registry[Index].TypeName)))
				{
					const ImVec2 ScreenPos = ImGui::GetMousePos();
					const ImVec2 CanvasPos = ed::ScreenToCanvas(ScreenPos);
					std::shared_ptr<INode> NewNode = Registry[Index].Make();
					const FNodeId NewId = Model.AddNode(NewNode, CanvasPos.x, CanvasPos.y);
					ed::SetNodePosition(ed::NodeId(NewId), CanvasPos);
					bChanged = true;
				}
			}

			// Subgraph asset dropped from the library panel (base level only —
			// importing registers the definition in the patch's map).
			if (const ImGuiPayload* SgPayload = ImGui::AcceptDragDropPayload(SubgraphAssetPayloadId);
				SgPayload != nullptr && bAtBase)
			{
				const std::string AssetPath(static_cast<const char*>(SgPayload->Data));
				if (auto Loaded = LoadSubgraph(AssetPath))
				{
					// Re-use an existing definition of the same name (so repeated
					// drops share one definition), otherwise register the import.
					std::shared_ptr<FSubgraphDefinition> Def =
						Model.FindSubgraphDefinition(Loaded->Name);
					if (!Def)
					{
						Def = Model.AddSubgraphDefinition(
							std::make_shared<FSubgraphDefinition>(std::move(*Loaded)));
					}
					SyncSubgraphBoundaries(*Def);

					const ImVec2 CanvasPos = ed::ScreenToCanvas(ImGui::GetMousePos());
					auto Instance = std::make_shared<FSubgraph>();
					Instance->SetDefinition(Def);
					const FNodeId NewId = Model.AddNode(Instance, CanvasPos.x, CanvasPos.y);
					ed::SetNodePosition(ed::NodeId(NewId), CanvasPos);
					bChanged = true;
				}
			}
			ImGui::EndDragDropTarget();
		}

		ed::SetCurrentEditor(nullptr);

		bFirstFrame = false;

		// Apply deferred subgraph navigation now that the canvas is closed, so
		// the active context / model swap before the next frame's Draw.
		if (PendingDive)
		{
			EnterSubgraph(PendingDive);
			PendingDive.reset();
		}
		else if (PendingPopTo >= 0)
		{
			PopToLevel(PendingPopTo);
		}
		PendingPopTo = -1;

		return bChanged;
	}

	void FGraphEditorPanel::DrawPropertyPanel(FGraphModel& PatchModel)
	{
		// Mirror Draw's active-level resolution so the property panel shows the
		// node selected in whichever level is open. History is disabled inside
		// subgraphs (deferred — see Draw).
		const bool bAtBase = SubgraphStack.empty();
		FGraphModel& Model = bAtBase ? PatchModel : *SubgraphStack.back()->Model;
		ed::EditorContext* const ActiveCtx = bAtBase ? this->Context : SubgraphStack.back()->Context;
		FEditHistory* const ActiveHistory = bAtBase ? this->History : nullptr;

		// When dived, the subgraph's pin-management panel sits above the
		// selected-node properties.
		if (!bAtBase && SubgraphStack.back()->Definition)
		{
			DrawSubgraphPinPanel(PatchModel, *SubgraphStack.back()->Definition);
			ImGui::Separator();
		}

		ed::SetCurrentEditor(ActiveCtx);
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
			// Inside a subgraph, the queued command can't reach the compiled
			// clone (different id) — flag a recompile so the edit applies.
			if (!bAtBase)
			{
				bSubgraphParamDirty = true;
			}
		};

		// Push a single SetParam edit-history entry with both old + new values.
		// Used by Bool/Choice (instantaneous) and Float (after slider release).
		auto PushSetParamEdit = [&](uint32_t Index, float OldVal, float NewVal)
		{
			if (ActiveHistory == nullptr || OldVal == NewVal) { return; }
			FEditCommand Cmd;
			Cmd.Type = EEditCommand::SetParam;
			Cmd.NodeId = Rec->Id;
			Cmd.ParamIndex = Index;
			Cmd.OldValue = OldVal;
			Cmd.NewValue = NewVal;
			ActiveHistory->Push(std::move(Cmd));
		};

		for (uint32_t I = 0; I < Infos.size(); ++I)
		{
			const FParamInfo& Info = Infos[I];
			if (Info.bHidden)
			{
				continue;  // hidden params (e.g. sequencer grid cells) are
				           // surfaced via custom UI hooks below.
			}
			// "Modulated" = a Control input is wired to this param's CV port.
			// In that state the slider becomes a read-only live readout of the
			// node's last computed value; user input is disabled because the
			// buffer overrides the param atomic anyway.
			const bool bModulated = (Info.ControlInputIndex >= 0)
				&& Model.HasIncomingLink(Rec->Id,
					static_cast<uint32_t>(Info.ControlInputIndex));
			float Value = bModulated
				? Rec->Node->GetLiveParamValue(I)
				: Rec->Node->GetParamValue(I);

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
					ImGuiSliderFlags Flags = Info.bLogarithmic ? ImGuiSliderFlags_Logarithmic : 0;
					if (bModulated) { Flags |= ImGuiSliderFlags_NoInput; }
					bool bChanged;
					if (Info.bUseInputBox)
					{
						// DragFloat with a per-pixel speed scaled to the
						// param's range — small ranges nudge in 0.001
						// increments, big ranges (filter Hz) move faster.
						const float Range = Info.MaxValue - Info.MinValue;
						const float Speed = (Range > 0.0f)
							? std::max(Range / 400.0f, 0.001f) : 0.01f;
						bChanged = ImGui::DragFloat(Info.Name.c_str(), &Value, Speed,
							Info.MinValue, Info.MaxValue, "%.3f", Flags);
					}
					else
					{
						bChanged = ImGui::SliderFloat(Info.Name.c_str(), &Value,
							Info.MinValue, Info.MaxValue, "%.3f", Flags);
					}
					if (bChanged)
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
		if (auto* CcNode = dynamic_cast<FMidiCC*>(Rec->Node.get()))
		{
			uint64_t LearnTarget = (LearnTargetParamIndex == LearnSentinel_MidiCcNode)
				? LearnTargetNodeId : 0;
			if (DrawMidiCCUI(*CcNode, Rec->Id, LearnTarget))
			{
				// Button click: either start learn (LearnTarget set) or cancel.
				LearnTargetNodeId = LearnTarget;
				LearnTargetParamIndex = LearnSentinel_MidiCcNode;
				LearnStartTimeSeconds = ImGui::GetTime();
			}
		}
		if (auto* Matrix = dynamic_cast<FModulationMatrix*>(Rec->Node.get()))
		{
			DrawModMatrixUI(*Matrix, Rec->Id, Model, Sink, ActiveHistory);
		}
		if (auto* Mic = dynamic_cast<FMicInput*>(Rec->Node.get()))
		{
			DrawMicInputUI(*Mic);
		}
	}

	void FGraphEditorPanel::DrawSubgraphPinPanel(FGraphModel& PatchModel, FSubgraphDefinition& Def)
	{
		// Locate the boundary nodes inside the definition.
		FNodeId InputsId = 0;
		FNodeId OutputsId = 0;
		for (const auto& [Id, Rec] : Def.InternalGraph.GetNodes())
		{
			if (!Rec.Node) { continue; }
			const std::string T = Rec.Node->GetTypeName();
			if (T == "_SubgraphInputs") { InputsId = Id; }
			else if (T == "_SubgraphOutputs") { OutputsId = Id; }
		}

		// Visit every instance of Def reachable from the open hierarchy (the
		// patch plus any open parent subgraph levels) so port-index fixups touch
		// all of them. The top level is Def's own graph — it can't contain an
		// instance of itself, so scanning it is harmless.
		auto ForEachInstance = [&](auto&& Fn)
		{
			auto Scan = [&](FGraphModel& M)
			{
				for (const auto& [Id, Rec] : M.GetNodes())
				{
					if (auto* Sub = dynamic_cast<FSubgraph*>(Rec.Node.get()))
					{
						if (Sub->GetDefinition().get() == &Def)
						{
							Fn(M, Id);
						}
					}
				}
			};
			Scan(PatchModel);
			for (auto& Level : SubgraphStack)
			{
				if (Level && Level->Model)
				{
					Scan(*Level->Model);
				}
			}
		};

		auto RemovePin = [&](bool bInput, uint32_t Idx)
		{
			std::vector<FSubgraphPin>& Pins = bInput ? Def.InputPins : Def.OutputPins;
			if (Idx >= Pins.size()) { return; }
			const FNodeId BoundaryId = bInput ? InputsId : OutputsId;
			// Input pins are the InputsBoundary's OUTPUT ports and the instance's
			// INPUT ports; output pins are the reverse.
			Def.InternalGraph.RemovePortAndShiftLinks(BoundaryId, bInput, Idx);
			ForEachInstance([&](FGraphModel& M, FNodeId Inst)
			{
				M.RemovePortAndShiftLinks(Inst, !bInput, Idx);
			});
			Pins.erase(Pins.begin() + Idx);
			SyncSubgraphBoundaries(Def);
			bSubgraphParamDirty = true;
		};

		ImGui::TextUnformatted("Subgraph Pins");

		// Editable definition name. Committed on focus-loss / Enter so we don't
		// re-key the map on every keystroke. Re-keying happens on the patch
		// model; instances share the definition pointer so their titles update.
		// A rename that collides with an existing definition is rejected (the
		// field reseeds to the current name next frame).
		char NameBuf[64];
		std::snprintf(NameBuf, sizeof(NameBuf), "%s", Def.Name.c_str());
		ImGui::SetNextItemWidth(200.0f);
		ImGui::InputText("Name##subgraph_name", NameBuf, sizeof(NameBuf));
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			const std::string NewName = NameBuf;
			if (!NewName.empty() && NewName != Def.Name)
			{
				PatchModel.RenameSubgraphDefinition(Def.Name, NewName);
			}
		}

		// Save the open definition as a reusable .nspg asset in the user dir.
		// The library panel picks it up on its next Refresh.
		if (ImGui::SmallButton("Save as Asset"))
		{
			std::error_code Ec;
			const std::filesystem::path Dir = GetUserSubgraphDir();
			std::filesystem::create_directories(Dir, Ec);
			const std::filesystem::path AssetPath = Dir / (Def.Name + ".nspg");
			SaveSubgraph(Def, AssetPath);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Writes %s.nspg to your user subgraphs folder.\n"
				"Refresh the Subgraph Library to see it.", Def.Name.c_str());
		}

		auto DrawPinList = [&](bool bInput)
		{
			std::vector<FSubgraphPin>& Pins = bInput ? Def.InputPins : Def.OutputPins;
			const FNodeId BoundaryId = bInput ? InputsId : OutputsId;
			const bool bBoundaryOutput = bInput;   // input pins = boundary outputs
			const bool bInstanceOutput = !bInput;  // input pins = instance inputs

			ImGui::SeparatorText(bInput ? "Inputs" : "Outputs");
			for (uint32_t I = 0; I < Pins.size(); ++I)
			{
				ImGui::PushID(static_cast<int>((bInput ? 0x10000u : 0x20000u) + I));

				ImGui::TextUnformatted(Pins[I].Type == EPortType::Control ? "Ctrl" : "Aud ");
				ImGui::SameLine();

				char Buf[64];
				std::snprintf(Buf, sizeof(Buf), "%s", Pins[I].Name.c_str());
				ImGui::SetNextItemWidth(120.0f);
				if (ImGui::InputText("##name", Buf, sizeof(Buf)))
				{
					Pins[I].Name = Buf;
					SyncSubgraphBoundaries(Def);  // label-only; no recompile
				}

				ImGui::SameLine();
				if (ImGui::ArrowButton("##up", ImGuiDir_Up) && I > 0)
				{
					Def.InternalGraph.SwapPortLinks(BoundaryId, bBoundaryOutput, I, I - 1);
					ForEachInstance([&](FGraphModel& M, FNodeId Inst)
					{
						M.SwapPortLinks(Inst, bInstanceOutput, I, I - 1);
					});
					std::swap(Pins[I], Pins[I - 1]);
					SyncSubgraphBoundaries(Def);
					bSubgraphParamDirty = true;
				}
				ImGui::SameLine();
				if (ImGui::ArrowButton("##down", ImGuiDir_Down) && I + 1 < Pins.size())
				{
					Def.InternalGraph.SwapPortLinks(BoundaryId, bBoundaryOutput, I, I + 1);
					ForEachInstance([&](FGraphModel& M, FNodeId Inst)
					{
						M.SwapPortLinks(Inst, bInstanceOutput, I, I + 1);
					});
					std::swap(Pins[I], Pins[I + 1]);
					SyncSubgraphBoundaries(Def);
					bSubgraphParamDirty = true;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("x"))
				{
					uint32_t LinkCount = Def.InternalGraph.CountPortLinks(BoundaryId, bBoundaryOutput, I);
					ForEachInstance([&](FGraphModel& M, FNodeId Inst)
					{
						LinkCount += M.CountPortLinks(Inst, bInstanceOutput, I);
					});
					bConfirmPinRemoveIsInput = bInput;
					ConfirmPinRemoveIndex = static_cast<int32_t>(I);
					ConfirmPinRemoveLinkCount = LinkCount;
					if (LinkCount == 0)
					{
						bPinRemoveConfirmed = true;  // applied after the lists draw
					}
					else
					{
						ImGui::OpenPopup("ConfirmPinRemove");
					}
				}

				ImGui::PopID();
			}

			// Add-pin row.
			int32_t& AddType = bInput ? AddInputPinType : AddOutputPinType;
			const char* TypeNames[] = { "Audio", "Control" };
			ImGui::PushID(bInput ? "addin" : "addout");
			ImGui::SetNextItemWidth(90.0f);
			ImGui::Combo("##type", &AddType, TypeNames, 2);
			ImGui::SameLine();
			if (ImGui::SmallButton(bInput ? "+ Add Input" : "+ Add Output"))
			{
				FSubgraphPin NewPin;
				NewPin.Name = (bInput ? "In" : "Out") + std::to_string(Pins.size() + 1);
				NewPin.Type = (AddType == 1) ? EPortType::Control : EPortType::Audio;
				Pins.push_back(std::move(NewPin));  // new port appended → no link fixup
				SyncSubgraphBoundaries(Def);
				bSubgraphParamDirty = true;
			}
			ImGui::PopID();
		};

		DrawPinList(true);
		DrawPinList(false);

		// Confirmation modal for removing a pin that still has links.
		if (ImGui::BeginPopupModal("ConfirmPinRemove", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("This pin has %u connected link(s).", ConfirmPinRemoveLinkCount);
			ImGui::TextUnformatted("Remove the pin and break them?");
			if (ImGui::Button("Remove"))
			{
				bPinRemoveConfirmed = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				ConfirmPinRemoveIndex = -1;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// Apply a deferred removal after both lists (and the modal) have drawn,
		// so the pin vector isn't mutated mid-iteration.
		if (bPinRemoveConfirmed && ConfirmPinRemoveIndex >= 0)
		{
			RemovePin(bConfirmPinRemoveIsInput, static_cast<uint32_t>(ConfirmPinRemoveIndex));
			ConfirmPinRemoveIndex = -1;
		}
		bPinRemoveConfirmed = false;
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
