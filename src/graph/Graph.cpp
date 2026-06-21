#include "graph/Graph.h"

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "dsp/MidiCC.h"
#include "dsp/Subgraph.h"
#include "dsp/VoiceAllocator.h"
#include "dsp/internal/SubgraphBoundary.h"
#include "dsp/internal/VoiceMixer.h"
#include "graph/EditHistory.h"
#include "graph/SubgraphDefinition.h"

namespace NodeSynth
{
	namespace
	{
		bool IsOutputNode(const INode& Node)
		{
			return std::string(Node.GetTypeName()) == "Output";
		}

		bool HasOutputAlready(const std::unordered_map<FNodeId, FNodeRecord>& Nodes)
		{
			for (const auto& [Id, Rec] : Nodes)
			{
				if (Rec.Node && IsOutputNode(*Rec.Node))
				{
					return true;
				}
			}
			return false;
		}
	}

	FNodeId FGraphModel::AddNode(std::shared_ptr<INode> Node, float X, float Y)
	{
		if (Node && IsOutputNode(*Node) && HasOutputAlready(Nodes))
		{
			return 0;  // singleton FOutput enforcement
		}
		const FNodeId Id = NextNodeId++;
		// Capture type name + per-voice flag (which is 0 at construction) so
		// the undo entry can recreate the same node on Undo/Redo replay.
		std::string TypeName = Node ? Node->GetTypeName() : std::string();
		Nodes.emplace(Id, FNodeRecord{ Id, std::move(Node), X, Y });
		if (IsRecordingHistory())
		{
			FEditCommand Cmd;
			Cmd.Type = EEditCommand::AddNode;
			Cmd.NodeId = Id;
			Cmd.NodeType = std::move(TypeName);
			Cmd.PosX = X;
			Cmd.PosY = Y;
			History->Push(std::move(Cmd));
		}
		return Id;
	}

	FNodeId FGraphModel::AddNodeWithId(FNodeId Id, std::shared_ptr<INode> Node, float X, float Y)
	{
		if (Id == 0 || Nodes.count(Id) > 0)
		{
			return 0;
		}
		if (Node && IsOutputNode(*Node) && HasOutputAlready(Nodes))
		{
			return 0;
		}
		std::string TypeName = Node ? Node->GetTypeName() : std::string();
		Nodes.emplace(Id, FNodeRecord{ Id, std::move(Node), X, Y });
		if (Id >= NextNodeId)
		{
			NextNodeId = Id + 1;
		}
		if (IsRecordingHistory())
		{
			FEditCommand Cmd;
			Cmd.Type = EEditCommand::AddNode;
			Cmd.NodeId = Id;
			Cmd.NodeType = std::move(TypeName);
			Cmd.PosX = X;
			Cmd.PosY = Y;
			History->Push(std::move(Cmd));
		}
		return Id;
	}

	void FGraphModel::RemoveNode(FNodeId Id)
	{
		auto It = Nodes.find(Id);
		if (It == Nodes.end())
		{
			return;
		}

		FEditCommand Cmd;
		const bool bRecord = IsRecordingHistory();
		if (bRecord)
		{
			Cmd.Type = EEditCommand::RemoveNode;
			Cmd.NodeId = Id;
			Cmd.NodeType = It->second.Node ? It->second.Node->GetTypeName() : std::string();
			Cmd.PosX = It->second.PositionX;
			Cmd.PosY = It->second.PositionY;
			Cmd.bPerVoice = It->second.bPerVoice;
			if (It->second.Node)
			{
				const auto Infos = It->second.Node->GetParamInfos();
				for (uint32_t I = 0; I < Infos.size(); ++I)
				{
					Cmd.Params.push_back({ Infos[I].Name, It->second.Node->GetParamValue(I) });
				}
			}
			for (const FLink& L : Links)
			{
				if (L.FromNode == Id || L.ToNode == Id)
				{
					Cmd.IncidentLinks.push_back({ L.Id, L.FromNode, L.FromPort, L.ToNode, L.ToPort });
				}
			}
		}

		Links.erase(std::remove_if(Links.begin(), Links.end(),
			[Id](const FLink& L) { return L.FromNode == Id || L.ToNode == Id; }),
			Links.end());

		// Capture and sweep MIDI mappings whose target was this node, so the
		// RemoveNode undo entry can restore them. Same shape as IncidentLinks.
		if (bRecord)
		{
			for (const FMidiMapping& M : MidiMappings)
			{
				if (M.NodeId == Id)
				{
					Cmd.IncidentMappings.push_back(M);
				}
			}
		}
		MidiMappings.erase(std::remove_if(MidiMappings.begin(), MidiMappings.end(),
			[Id](const FMidiMapping& M) { return M.NodeId == Id; }),
			MidiMappings.end());

		Nodes.erase(Id);

		if (bRecord)
		{
			History->Push(std::move(Cmd));
		}
	}

	FNodeRecord* FGraphModel::FindNode(FNodeId Id)
	{
		auto It = Nodes.find(Id);
		return (It != Nodes.end()) ? &It->second : nullptr;
	}

	std::shared_ptr<FSubgraphDefinition> FGraphModel::AddSubgraphDefinition(
		std::shared_ptr<FSubgraphDefinition> Def)
	{
		if (!Def)
		{
			return nullptr;
		}
		std::shared_ptr<FSubgraphDefinition> Stored = Def;
		Subgraphs[Def->Name] = std::move(Def);
		return Stored;
	}

	std::shared_ptr<FSubgraphDefinition> FGraphModel::FindSubgraphDefinition(const std::string& Name) const
	{
		auto It = Subgraphs.find(Name);
		return It != Subgraphs.end() ? It->second : nullptr;
	}

	bool FGraphModel::RenameSubgraphDefinition(const std::string& OldName, const std::string& NewName)
	{
		if (NewName.empty() || NewName == OldName || Subgraphs.count(NewName) > 0)
		{
			return false;
		}
		auto It = Subgraphs.find(OldName);
		if (It == Subgraphs.end())
		{
			return false;
		}
		std::shared_ptr<FSubgraphDefinition> Def = It->second;
		Subgraphs.erase(It);
		Def->Name = NewName;
		Subgraphs[NewName] = std::move(Def);
		return true;
	}

	bool FGraphModel::HasIncomingLink(FNodeId Node, uint32_t Port) const
	{
		for (const FLink& L : Links)
		{
			if (L.ToNode == Node && L.ToPort == Port) { return true; }
		}
		return false;
	}

	bool FGraphModel::SetNodePerVoice(FNodeId Id, bool bPerVoice)
	{
		auto It = Nodes.find(Id);
		if (It == Nodes.end())
		{
			return false;
		}
		// Cloneability test: a node whose Clone() returns nullptr can't be
		// duplicated per voice (RtMidi handles, UI state, singleton sinks).
		if (bPerVoice && It->second.Node && It->second.Node->Clone() == nullptr)
		{
			return false;
		}
		const bool bWas = It->second.bPerVoice;
		It->second.bPerVoice = bPerVoice;
		if (bWas != bPerVoice && IsRecordingHistory())
		{
			FEditCommand Cmd;
			Cmd.Type = EEditCommand::SetNodePerVoice;
			Cmd.NodeId = Id;
			Cmd.OldPerVoice = bWas;
			Cmd.NewPerVoice = bPerVoice;
			History->Push(std::move(Cmd));
		}
		return true;
	}

	void FGraphModel::RemovePortAndShiftLinks(FNodeId Node, bool bOutputPort, uint32_t RemovedPort)
	{
		Links.erase(std::remove_if(Links.begin(), Links.end(),
			[&](const FLink& L)
			{
				const FNodeId N = bOutputPort ? L.FromNode : L.ToNode;
				const uint32_t P = bOutputPort ? L.FromPort : L.ToPort;
				return N == Node && P == RemovedPort;
			}),
			Links.end());

		for (FLink& L : Links)
		{
			if (bOutputPort && L.FromNode == Node && L.FromPort > RemovedPort)
			{
				--L.FromPort;
			}
			else if (!bOutputPort && L.ToNode == Node && L.ToPort > RemovedPort)
			{
				--L.ToPort;
			}
		}
	}

	void FGraphModel::SwapPortLinks(FNodeId Node, bool bOutputPort, uint32_t PortA, uint32_t PortB)
	{
		for (FLink& L : Links)
		{
			if (bOutputPort && L.FromNode == Node)
			{
				if (L.FromPort == PortA) { L.FromPort = PortB; }
				else if (L.FromPort == PortB) { L.FromPort = PortA; }
			}
			else if (!bOutputPort && L.ToNode == Node)
			{
				if (L.ToPort == PortA) { L.ToPort = PortB; }
				else if (L.ToPort == PortB) { L.ToPort = PortA; }
			}
		}
	}

	uint32_t FGraphModel::CountPortLinks(FNodeId Node, bool bOutputPort, uint32_t Port) const
	{
		uint32_t Count = 0;
		for (const FLink& L : Links)
		{
			const FNodeId N = bOutputPort ? L.FromNode : L.ToNode;
			const uint32_t P = bOutputPort ? L.FromPort : L.ToPort;
			if (N == Node && P == Port)
			{
				++Count;
			}
		}
		return Count;
	}

	void FGraphModel::AddMidiMapping(const FMidiMapping& Mapping)
	{
		// Replace any existing mapping on the same (Channel, Cc) pair. Wrap
		// removal + addition in a composite history entry so undo restores
		// both in one step.
		const bool bRecord = IsRecordingHistory();
		const bool bGroup = bRecord;
		if (bGroup) { History->BeginComposite(); }

		auto It = std::find_if(MidiMappings.begin(), MidiMappings.end(),
			[&](const FMidiMapping& M) { return M.Channel == Mapping.Channel && M.Cc == Mapping.Cc; });
		if (It != MidiMappings.end())
		{
			if (bRecord)
			{
				FEditCommand Cmd;
				Cmd.Type = EEditCommand::RemoveMidiMapping;
				Cmd.MidiChannel = It->Channel;
				Cmd.MidiCc = It->Cc;
				Cmd.NodeId = It->NodeId;
				Cmd.ParamIndex = It->ParamIndex;
				History->Push(std::move(Cmd));
			}
			MidiMappings.erase(It);
		}

		MidiMappings.push_back(Mapping);
		if (bRecord)
		{
			FEditCommand Cmd;
			Cmd.Type = EEditCommand::AddMidiMapping;
			Cmd.MidiChannel = Mapping.Channel;
			Cmd.MidiCc = Mapping.Cc;
			Cmd.NodeId = Mapping.NodeId;
			Cmd.ParamIndex = Mapping.ParamIndex;
			History->Push(std::move(Cmd));
		}

		if (bGroup) { History->EndComposite(); }
	}

	void FGraphModel::RemoveMidiMapping(uint8_t Channel, uint8_t Cc)
	{
		auto It = std::find_if(MidiMappings.begin(), MidiMappings.end(),
			[&](const FMidiMapping& M) { return M.Channel == Channel && M.Cc == Cc; });
		if (It == MidiMappings.end())
		{
			return;
		}
		if (IsRecordingHistory())
		{
			FEditCommand Cmd;
			Cmd.Type = EEditCommand::RemoveMidiMapping;
			Cmd.MidiChannel = It->Channel;
			Cmd.MidiCc = It->Cc;
			Cmd.NodeId = It->NodeId;
			Cmd.ParamIndex = It->ParamIndex;
			History->Push(std::move(Cmd));
		}
		MidiMappings.erase(It);
	}

	const FMidiMapping* FGraphModel::FindMidiMapping(FNodeId NodeId, uint32_t ParamIndex) const
	{
		for (const FMidiMapping& M : MidiMappings)
		{
			if (M.NodeId == NodeId && M.ParamIndex == ParamIndex) { return &M; }
		}
		return nullptr;
	}

	std::string FGraphModel::ValidateLinkPolyphony(FNodeId FromNode, uint32_t FromPort, FNodeId ToNode) const
	{
		// Mirror the rule enforced in Compile step 4: a polyphonic Control
		// output (from a VoiceAllocator or a per-voice node) into a mono node
		// is rejected because there's no mixdown semantics for control rate.
		auto FromIt = Nodes.find(FromNode);
		auto ToIt = Nodes.find(ToNode);
		if (FromIt == Nodes.end() || ToIt == Nodes.end())
		{
			return std::string();
		}
		const INode* FromNodePtr = FromIt->second.Node.get();
		const INode* ToNodePtr = ToIt->second.Node.get();
		if (!FromNodePtr || !ToNodePtr)
		{
			return std::string();
		}
		const bool bFromIsVoiceAlloc = dynamic_cast<const FVoiceAllocator*>(FromNodePtr) != nullptr;
		const bool bToIsVoiceAlloc = dynamic_cast<const FVoiceAllocator*>(ToNodePtr) != nullptr;
		const bool bFromPoly = bFromIsVoiceAlloc || FromIt->second.bPerVoice;
		const bool bToMono = !bToIsVoiceAlloc && !ToIt->second.bPerVoice;
		if (!bFromPoly || !bToMono)
		{
			return std::string();
		}
		const auto FromPorts = FromNodePtr->GetOutputPorts();
		if (FromPort >= FromPorts.size() || FromPorts[FromPort].Type != EPortType::Control)
		{
			return std::string();
		}
		return "Per-voice Control output → mono input. Mark the destination "
			"per-voice (right-click → Per-voice) or break the link.";
	}

	namespace
	{
		bool WouldCreateCycle(const std::vector<FLink>& Links, FNodeId FromNode, FNodeId ToNode)
		{
			// DFS along existing edges starting at ToNode. If we reach FromNode,
			// a new FromNode -> ToNode edge would close a loop.
			std::unordered_set<FNodeId> Visited;
			std::vector<FNodeId> Stack{ ToNode };
			while (!Stack.empty())
			{
				FNodeId Current = Stack.back();
				Stack.pop_back();
				if (Current == FromNode)
				{
					return true;
				}
				if (!Visited.insert(Current).second)
				{
					continue;
				}
				for (const FLink& L : Links)
				{
					if (L.FromNode == Current)
					{
						Stack.push_back(L.ToNode);
					}
				}
			}
			return false;
		}
	}

	FLinkId FGraphModel::AddLink(FNodeId FromNode, uint32_t FromPort, FNodeId ToNode, uint32_t ToPort)
	{
		FNodeRecord* From = FindNode(FromNode);
		FNodeRecord* To = FindNode(ToNode);
		if (!From || !To)
		{
			return 0;
		}

		const auto OutPorts = From->Node->GetOutputPorts();
		const auto InPorts = To->Node->GetInputPorts();
		if (FromPort >= OutPorts.size() || ToPort >= InPorts.size())
		{
			return 0;
		}
		if (OutPorts[FromPort].Type != InPorts[ToPort].Type)
		{
			return 0;
		}
		if (FromNode == ToNode)
		{
			return 0;
		}

		// An input port takes one connection at a time. If something already
		// terminates here, capture it so undo can restore it as part of a
		// composite history entry (otherwise replacing a link silently loses
		// the prior connection from the undo stack).
		FLink Displaced{};
		bool bDisplaced = false;
		for (const FLink& L : Links)
		{
			if (L.ToNode == ToNode && L.ToPort == ToPort)
			{
				Displaced = L;
				bDisplaced = true;
				break;
			}
		}
		Links.erase(std::remove_if(Links.begin(), Links.end(),
			[ToNode, ToPort](const FLink& L) { return L.ToNode == ToNode && L.ToPort == ToPort; }),
			Links.end());

		if (WouldCreateCycle(Links, FromNode, ToNode))
		{
			// Restore the displaced link before bailing — caller's expectation
			// is "AddLink failed, graph unchanged".
			if (bDisplaced)
			{
				Links.push_back(Displaced);
			}
			return 0;
		}

		const FLinkId Id = NextLinkId++;
		Links.push_back(FLink{ Id, FromNode, FromPort, ToNode, ToPort });

		if (IsRecordingHistory())
		{
			// Composite when the new link displaced an existing one, so a
			// single Undo restores both endpoints (remove the new one + put
			// the old one back).
			const bool bComposite = bDisplaced;
			if (bComposite) { History->BeginComposite(); }
			if (bDisplaced)
			{
				FEditCommand RemoveCmd;
				RemoveCmd.Type = EEditCommand::RemoveLink;
				RemoveCmd.LinkId = Displaced.Id;
				RemoveCmd.FromNode = Displaced.FromNode;
				RemoveCmd.FromPort = Displaced.FromPort;
				RemoveCmd.ToNode = Displaced.ToNode;
				RemoveCmd.ToPort = Displaced.ToPort;
				History->Push(std::move(RemoveCmd));
			}
			FEditCommand AddCmd;
			AddCmd.Type = EEditCommand::AddLink;
			AddCmd.LinkId = Id;
			AddCmd.FromNode = FromNode;
			AddCmd.FromPort = FromPort;
			AddCmd.ToNode = ToNode;
			AddCmd.ToPort = ToPort;
			History->Push(std::move(AddCmd));
			if (bComposite) { History->EndComposite(); }
		}
		return Id;
	}

	FLinkId FGraphModel::AddLinkWithId(FLinkId Id, FNodeId FromNode, uint32_t FromPort, FNodeId ToNode, uint32_t ToPort)
	{
		if (Id == 0)
		{
			return 0;
		}
		for (const FLink& L : Links)
		{
			if (L.Id == Id) { return 0; }  // already in use
		}
		FNodeRecord* From = FindNode(FromNode);
		FNodeRecord* To = FindNode(ToNode);
		if (!From || !To) { return 0; }
		const auto OutPorts = From->Node->GetOutputPorts();
		const auto InPorts = To->Node->GetInputPorts();
		if (FromPort >= OutPorts.size() || ToPort >= InPorts.size()) { return 0; }
		if (OutPorts[FromPort].Type != InPorts[ToPort].Type) { return 0; }
		if (FromNode == ToNode) { return 0; }
		// Drop any conflicting same-input-port link.
		Links.erase(std::remove_if(Links.begin(), Links.end(),
			[ToNode, ToPort](const FLink& L) { return L.ToNode == ToNode && L.ToPort == ToPort; }),
			Links.end());
		if (WouldCreateCycle(Links, FromNode, ToNode)) { return 0; }
		Links.push_back(FLink{ Id, FromNode, FromPort, ToNode, ToPort });
		if (Id >= NextLinkId)
		{
			NextLinkId = Id + 1;
		}
		// AddLinkWithId is only used by undo/redo replay, so by design
		// IsRecordingHistory() is false here. We don't push.
		return Id;
	}

	void FGraphModel::RemoveLink(FLinkId Id)
	{
		FEditCommand Cmd;
		const bool bRecord = IsRecordingHistory();
		if (bRecord)
		{
			for (const FLink& L : Links)
			{
				if (L.Id == Id)
				{
					Cmd.Type = EEditCommand::RemoveLink;
					Cmd.LinkId = L.Id;
					Cmd.FromNode = L.FromNode;
					Cmd.FromPort = L.FromPort;
					Cmd.ToNode = L.ToNode;
					Cmd.ToPort = L.ToPort;
					break;
				}
			}
		}
		const size_t Before = Links.size();
		Links.erase(std::remove_if(Links.begin(), Links.end(),
			[Id](const FLink& L) { return L.Id == Id; }),
			Links.end());
		if (bRecord && Links.size() != Before)
		{
			History->Push(std::move(Cmd));
		}
	}

	namespace
	{
		bool IsSubgraphInstance(const INode& Node)
		{
			return std::string(Node.GetTypeName()) == "Subgraph";
		}

		// Node types that may not live inside a subgraph: the patch sink, the
		// voice allocator, and the (deprecated) in-graph note-input types.
		bool IsForbiddenInSubgraph(const INode& Node)
		{
			const std::string T = Node.GetTypeName();
			return T == "Output" || T == "VoiceAllocator" || T == "MIDI" || T == "VirtualKbd";
		}

		constexpr size_t MaxSubgraphDepth = 8;

		// DFS over the definition-reference tree. Detects transitive recursion
		// (a definition that contains itself), depth-limit violations, and
		// forbidden internal node types. Returns an error string, or empty if
		// the definition (and everything it nests) is valid.
		std::string ValidateSubgraphDefinition(const FSubgraphDefinition& Def,
			std::vector<std::string>& Stack)
		{
			for (const std::string& Name : Stack)
			{
				if (Name == Def.Name)
				{
					return "Subgraph '" + Def.Name + "' is recursive — it transitively "
						"contains an instance of itself.";
				}
			}
			if (Stack.size() >= MaxSubgraphDepth)
			{
				return "Subgraph nesting exceeds the " + std::to_string(MaxSubgraphDepth)
					+ "-level limit at '" + Def.Name + "'.";
			}

			Stack.push_back(Def.Name);
			for (const auto& [Id, Rec] : Def.InternalGraph.GetNodes())
			{
				if (!Rec.Node)
				{
					continue;
				}
				if (IsForbiddenInSubgraph(*Rec.Node))
				{
					return "Subgraph '" + Def.Name + "' contains forbidden node type '"
						+ Rec.Node->GetTypeName() + "'. Subgraphs can't contain the patch "
						"sink, voice allocator, or note-input nodes.";
				}
				if (auto* Sub = dynamic_cast<FSubgraph*>(Rec.Node.get()))
				{
					if (Sub->GetDefinition())
					{
						std::string Err = ValidateSubgraphDefinition(*Sub->GetDefinition(), Stack);
						if (!Err.empty())
						{
							return Err;
						}
					}
				}
			}
			Stack.pop_back();
			return std::string();
		}

		// Macro-inlines one FSubgraph instance (SId) into WorkNodes/WorkLinks
		// following the §1.1 algorithm: clone the internal non-boundary nodes
		// with fresh ids, rewire internal links, then splice external links
		// through the boundary pins. The instance's per-voice flag propagates
		// onto every cloned internal node. Boundary nodes are never cloned —
		// they exist only to identify pin endpoints.
		void ExpandOneSubgraph(std::unordered_map<FNodeId, FNodeRecord>& WorkNodes,
			std::vector<FLink>& WorkLinks, FNodeId SId,
			FNodeId& NextFreshNode, FLinkId& NextFreshLink)
		{
			const FNodeRecord SRec = WorkNodes.at(SId);  // copy — we erase SId below
			auto* Sub = dynamic_cast<FSubgraph*>(SRec.Node.get());
			const FSubgraphDefinition& Def = *Sub->GetDefinition();
			const bool bPerVoice = SRec.bPerVoice;

			// 1. Clone internal non-boundary nodes with fresh ids.
			std::unordered_map<FNodeId, FNodeId> IdMap;
			FNodeId InputsId = 0;
			FNodeId OutputsId = 0;
			for (const auto& [Iid, Irec] : Def.InternalGraph.GetNodes())
			{
				if (!Irec.Node)
				{
					continue;
				}
				const std::string T = Irec.Node->GetTypeName();
				if (T == "_SubgraphInputs") { InputsId = Iid; continue; }
				if (T == "_SubgraphOutputs") { OutputsId = Iid; continue; }

				std::shared_ptr<INode> Clone = Irec.Node->Clone();
				if (!Clone)
				{
					continue;  // validated as cloneable already; defensive
				}
				Clone->MasterMirror = nullptr;  // it IS the master in the flat graph
				const FNodeId Fid = NextFreshNode++;
				IdMap[Iid] = Fid;
				WorkNodes.emplace(Fid, FNodeRecord{ Fid, std::move(Clone),
					Irec.PositionX, Irec.PositionY, bPerVoice || Irec.bPerVoice });
			}

			// 2. External drivers of the instance's input pins / consumers of
			// its output pins (from links touching SId in the working graph).
			std::unordered_map<uint32_t, std::pair<FNodeId, uint32_t>> InputPinDriver;
			std::vector<std::tuple<uint32_t, FNodeId, uint32_t>> OutputConsumers;
			for (const FLink& L : WorkLinks)
			{
				if (L.ToNode == SId)
				{
					InputPinDriver[L.ToPort] = { L.FromNode, L.FromPort };
				}
				if (L.FromNode == SId)
				{
					OutputConsumers.push_back({ L.FromPort, L.ToNode, L.ToPort });
				}
			}

			auto AddLink = [&](FNodeId F, uint32_t Fp, FNodeId T, uint32_t Tp)
			{
				WorkLinks.push_back(FLink{ NextFreshLink++, F, Fp, T, Tp });
			};

			// 3. Walk internal links, classifying each endpoint against the two
			// boundary nodes.
			std::unordered_map<uint32_t, std::pair<FNodeId, uint32_t>> OutputPinSource;
			std::unordered_map<uint32_t, uint32_t> PassthroughOutToIn;
			for (const FLink& L : Def.InternalGraph.GetLinks())
			{
				const bool bFromInputs = (L.FromNode == InputsId);
				const bool bToOutputs = (L.ToNode == OutputsId);
				if (!bFromInputs && !bToOutputs)
				{
					auto Fit = IdMap.find(L.FromNode);
					auto Tit = IdMap.find(L.ToNode);
					if (Fit != IdMap.end() && Tit != IdMap.end())
					{
						AddLink(Fit->second, L.FromPort, Tit->second, L.ToPort);
					}
				}
				else if (bFromInputs && !bToOutputs)
				{
					// Input pin L.FromPort drives an internal consumer.
					auto Tit = IdMap.find(L.ToNode);
					auto Dit = InputPinDriver.find(L.FromPort);
					if (Tit != IdMap.end() && Dit != InputPinDriver.end())
					{
						AddLink(Dit->second.first, Dit->second.second, Tit->second, L.ToPort);
					}
				}
				else if (!bFromInputs && bToOutputs)
				{
					// Internal producer feeds output pin L.ToPort.
					auto Fit = IdMap.find(L.FromNode);
					if (Fit != IdMap.end())
					{
						OutputPinSource[L.ToPort] = { Fit->second, L.FromPort };
					}
				}
				else
				{
					// Passthrough: input pin wired straight to an output pin.
					PassthroughOutToIn[L.ToPort] = L.FromPort;
				}
			}

			// 4. Connect output pins to the instance's external consumers.
			for (const auto& [PinJ, Consumer, Port] : OutputConsumers)
			{
				auto Sit = OutputPinSource.find(PinJ);
				if (Sit != OutputPinSource.end())
				{
					AddLink(Sit->second.first, Sit->second.second, Consumer, Port);
					continue;
				}
				auto Pit = PassthroughOutToIn.find(PinJ);
				if (Pit != PassthroughOutToIn.end())
				{
					auto Dit = InputPinDriver.find(Pit->second);
					if (Dit != InputPinDriver.end())
					{
						AddLink(Dit->second.first, Dit->second.second, Consumer, Port);
					}
				}
			}

			// 5. Drop the instance and every link incident to it.
			WorkLinks.erase(std::remove_if(WorkLinks.begin(), WorkLinks.end(),
				[SId](const FLink& L) { return L.FromNode == SId || L.ToNode == SId; }),
				WorkLinks.end());
			WorkNodes.erase(SId);
		}
	}

	bool FGraphModel::ExpandSubgraphs(std::unordered_map<FNodeId, FNodeRecord>& WorkNodes,
		std::vector<FLink>& WorkLinks)
	{
		// Fast path: nothing to do if there are no subgraph instances.
		bool bAny = false;
		for (const auto& [Id, Rec] : WorkNodes)
		{
			if (Rec.Node && IsSubgraphInstance(*Rec.Node))
			{
				bAny = true;
				break;
			}
		}
		if (!bAny)
		{
			return true;
		}

		// Validate every top-level instance's definition tree up front
		// (recursion / depth / forbidden types) before mutating anything.
		for (const auto& [Id, Rec] : WorkNodes)
		{
			auto* Sub = dynamic_cast<FSubgraph*>(Rec.Node.get());
			if (!Sub)
			{
				continue;
			}
			if (!Sub->GetDefinition())
			{
				LastCompileError.bHasError = true;
				LastCompileError.Message = "Subgraph instance has no definition bound.";
				LastCompileError.ToNode = Id;
				return false;
			}
			std::vector<std::string> Stack;
			std::string Err = ValidateSubgraphDefinition(*Sub->GetDefinition(), Stack);
			if (!Err.empty())
			{
				std::fprintf(stderr, "Compile: %s\n", Err.c_str());
				LastCompileError.bHasError = true;
				LastCompileError.Message = std::move(Err);
				LastCompileError.ToNode = Id;
				return false;
			}
		}

		// Fresh-id counters: start above every id already present so cloned
		// internal nodes / links never collide with the parent graph (§3).
		FNodeId NextFreshNode = NextNodeId;
		for (const auto& [Id, Rec] : WorkNodes)
		{
			NextFreshNode = std::max(NextFreshNode, Id + 1);
		}
		FLinkId NextFreshLink = NextLinkId;
		for (const FLink& L : WorkLinks)
		{
			NextFreshLink = std::max(NextFreshLink, L.Id + 1);
		}

		// Flatten iteratively: each pass expands one instance. Nested instances
		// surface as fresh nodes and get expanded on later passes. Recursion is
		// already rejected, so the definition-reference DAG is finite and this
		// terminates; the guard is a backstop against pathological inputs.
		size_t Guard = 0;
		while (true)
		{
			FNodeId SId = 0;
			for (const auto& [Id, Rec] : WorkNodes)
			{
				if (Rec.Node && IsSubgraphInstance(*Rec.Node))
				{
					SId = Id;
					break;
				}
			}
			if (SId == 0)
			{
				break;
			}
			if (++Guard > 10000)
			{
				LastCompileError.bHasError = true;
				LastCompileError.Message = "Subgraph expansion exceeded the safety limit.";
				return false;
			}
			ExpandOneSubgraph(WorkNodes, WorkLinks, SId, NextFreshNode, NextFreshLink);
		}
		return true;
	}

	std::shared_ptr<FAudioGraph> FGraphModel::Compile(double SampleRate)
	{
		LastCompileError = FCompileError{};  // clear at the start of every Compile

		// Subgraph pre-pass. Work on copies so the user-visible model is never
		// mutated; expansion macro-inlines every FSubgraph instance into its
		// internal nodes (docs/PLAN-SUBGRAPHS.md §1.1) before the normal
		// partition / DFS / plumb runs on the flattened graph. The common case
		// (no subgraphs) is a cheap copy + early return inside ExpandSubgraphs.
		std::unordered_map<FNodeId, FNodeRecord> WorkNodes = Nodes;
		std::vector<FLink> WorkLinks = Links;
		if (!ExpandSubgraphs(WorkNodes, WorkLinks))
		{
			return std::make_shared<FAudioGraph>();  // LastCompileError is set
		}
		return CompileFlattened(WorkNodes, WorkLinks, SampleRate);
	}

	std::shared_ptr<FAudioGraph> FGraphModel::CompileFlattened(
		const std::unordered_map<FNodeId, FNodeRecord>& Nodes,
		const std::vector<FLink>& Links,
		double SampleRate)
	{
		auto Graph = std::make_shared<FAudioGraph>();

		// -- Step 1: locate the output sink --------------------------------------
		const FNodeRecord* OutputRec = nullptr;
		for (const auto& [Id, Rec] : Nodes)
		{
			if (std::string(Rec.Node->GetTypeName()) == "Output")
			{
				OutputRec = &Rec;
				break;
			}
		}
		if (!OutputRec)
		{
			return Graph;
		}

		// -- Step 2: reverse-DFS from the output, producers-first order of ids --
		std::unordered_set<FNodeId> Visited;
		std::vector<FNodeId> Order;
		std::function<void(FNodeId)> Visit = [&](FNodeId Id)
		{
			if (!Visited.insert(Id).second)
			{
				return;
			}
			for (const FLink& L : Links)
			{
				if (L.ToNode == Id)
				{
					Visit(L.FromNode);
				}
			}
			if (Nodes.count(Id) > 0)
			{
				Order.push_back(Id);
			}
		};
		Visit(OutputRec->Id);

		// -- Step 3: classify each visited node ----------------------------------
		enum class EClass : uint8_t { Mono, PerVoice, VoiceAlloc };
		std::unordered_map<FNodeId, EClass> Class;
		Class.reserve(Order.size());
		for (FNodeId Id : Order)
		{
			const FNodeRecord& Rec = Nodes.at(Id);
			if (dynamic_cast<FVoiceAllocator*>(Rec.Node.get()))
			{
				Class[Id] = EClass::VoiceAlloc;
			}
			else if (Rec.bPerVoice)
			{
				Class[Id] = EClass::PerVoice;
			}
			else
			{
				Class[Id] = EClass::Mono;
			}
		}

		auto IsPolyClass = [](EClass C) {
			return C == EClass::PerVoice || C == EClass::VoiceAlloc;
		};

		// -- Step 4: validate edges. Per-voice → mono Control is rejected.
		// Returns an empty graph on validation failure; the audio thread sees
		// silence until the user fixes the routing. UI surfaces the error
		// via the empty-snapshot fallback.
		for (const FLink& L : Links)
		{
			if (Visited.count(L.FromNode) == 0 || Visited.count(L.ToNode) == 0)
			{
				continue;
			}
			const EClass FromC = Class.at(L.FromNode);
			const EClass ToC = Class.at(L.ToNode);
			if (IsPolyClass(FromC) && ToC == EClass::Mono)
			{
				const auto FromPorts = Nodes.at(L.FromNode).Node->GetOutputPorts();
				if (L.FromPort < FromPorts.size()
					&& FromPorts[L.FromPort].Type == EPortType::Control)
				{
					std::fprintf(stderr,
						"Compile: rejected per-voice → mono Control link (%llu:%u → %llu:%u). "
						"Mark the destination per-voice or break the link.\n",
						static_cast<unsigned long long>(L.FromNode), L.FromPort,
						static_cast<unsigned long long>(L.ToNode), L.ToPort);
					LastCompileError.bHasError = true;
					LastCompileError.Message =
						"Polyphonic Control output → mono Control input. "
						"Mark the destination per-voice (right-click the node → Per-voice) or break the link.";
					LastCompileError.FromNode = L.FromNode;
					LastCompileError.FromPort = L.FromPort;
					LastCompileError.ToNode = L.ToNode;
					LastCompileError.ToPort = L.ToPort;
					return std::make_shared<FAudioGraph>();
				}
			}
		}

		// -- Step 5: clone per-voice nodes ---------------------------------------
		constexpr size_t NumVoices = FVoiceAllocator::MaxVoices;
		std::unordered_map<FNodeId, std::vector<std::shared_ptr<INode>>> Clones;
		for (FNodeId Id : Order)
		{
			if (Class.at(Id) != EClass::PerVoice)
			{
				continue;
			}
			std::vector<std::shared_ptr<INode>> VoiceClones;
			VoiceClones.reserve(NumVoices);
			for (size_t V = 0; V < NumVoices; ++V)
			{
				auto Clone = Nodes.at(Id).Node->Clone();
				if (!Clone)
				{
					std::fprintf(stderr,
						"Compile: per-voice flag set on a non-cloneable node id %llu — "
						"skipping per-voice path, treating as mono.\n",
						static_cast<unsigned long long>(Id));
					Class[Id] = EClass::Mono;
					VoiceClones.clear();
					break;
				}
				// Stamp the clone's voice index so its Process knows which
				// slot of the master's live-value arrays to write.
				Clone->VoiceIndex = static_cast<int32_t>(V);
				VoiceClones.push_back(std::move(Clone));
			}
			if (!VoiceClones.empty())
			{
				Clones.emplace(Id, std::move(VoiceClones));
			}
		}

		// -- Step 6: synthesize voice mixers for per-voice → mono Audio links ---
		// One mixer per (FromNode, FromPort) — multiple destinations can share.
		std::map<std::pair<FNodeId, uint32_t>, std::shared_ptr<Internal::FVoiceMixer>> Mixers;
		auto GetVoiceSourceBuffer = [&](FNodeId FromId, uint32_t FromPort, size_t VoiceIdx) -> float*
		{
			const EClass FromC = Class.at(FromId);
			if (FromC == EClass::VoiceAlloc)
			{
				auto* Alloc = static_cast<FVoiceAllocator*>(Nodes.at(FromId).Node.get());
				return Alloc->GetVoiceOutputBuffer(FromPort, VoiceIdx);
			}
			if (FromC == EClass::PerVoice)
			{
				return Clones.at(FromId)[VoiceIdx]->GetOutputBuffer(FromPort);
			}
			return nullptr;
		};

		for (const FLink& L : Links)
		{
			if (Visited.count(L.FromNode) == 0 || Visited.count(L.ToNode) == 0)
			{
				continue;
			}
			const EClass FromC = Class.at(L.FromNode);
			const EClass ToC = Class.at(L.ToNode);
			if (!IsPolyClass(FromC) || ToC != EClass::Mono)
			{
				continue;
			}
			const auto FromPorts = Nodes.at(L.FromNode).Node->GetOutputPorts();
			if (L.FromPort >= FromPorts.size()
				|| FromPorts[L.FromPort].Type != EPortType::Audio)
			{
				continue;  // Control was rejected in Step 4
			}
			auto Key = std::make_pair(L.FromNode, L.FromPort);
			if (Mixers.find(Key) == Mixers.end())
			{
				auto Mixer = std::make_shared<Internal::FVoiceMixer>();
				Mixer->Prepare(SampleRate);
				for (size_t V = 0; V < NumVoices; ++V)
				{
					float* VBuf = GetVoiceSourceBuffer(L.FromNode, L.FromPort, V);
					Mixer->SetInputBuffer(static_cast<uint32_t>(V), VBuf, 0);
					Mixer->SetInputBuffer(static_cast<uint32_t>(V), VBuf, 1);
				}
				Mixers.emplace(Key, std::move(Mixer));
			}
		}

		// -- Step 7: build OrderedNodes with clones substituted and mixers
		// inserted right after the per-voice nodes whose audio they sum.
		std::vector<std::shared_ptr<INode>> OrderedNodes;
		for (FNodeId Id : Order)
		{
			const EClass C = Class.at(Id);
			if (C == EClass::PerVoice && Clones.count(Id) > 0)
			{
				for (auto& Clone : Clones.at(Id))
				{
					Clone->Prepare(SampleRate);
					const uint32_t NumIn = static_cast<uint32_t>(Clone->GetInputPorts().size());
					for (uint32_t I = 0; I < NumIn; ++I)
					{
						Clone->SetInputBuffer(I, nullptr, 0);
						Clone->SetInputBuffer(I, nullptr, 1);
					}
					OrderedNodes.push_back(Clone);
				}
			}
			else
			{
				auto& Node = Nodes.at(Id).Node;
				Node->Prepare(SampleRate);
				const uint32_t NumIn = static_cast<uint32_t>(Node->GetInputPorts().size());
				for (uint32_t I = 0; I < NumIn; ++I)
				{
					Node->SetInputBuffer(I, nullptr, 0);
					Node->SetInputBuffer(I, nullptr, 1);
				}
				OrderedNodes.push_back(Node);
			}

			// Append any mixers sourced from this node so they run after its
			// (now-completed) per-voice clones and before the mono consumer
			// they feed (which appears later in Order).
			for (auto& [Key, Mixer] : Mixers)
			{
				if (Key.first == Id)
				{
					OrderedNodes.push_back(Mixer);
				}
			}
		}

		// -- Step 8: wire all links ----------------------------------------------
		for (const FLink& L : Links)
		{
			if (Visited.count(L.FromNode) == 0 || Visited.count(L.ToNode) == 0)
			{
				continue;
			}
			const EClass FromC = Class.at(L.FromNode);
			const EClass ToC = Class.at(L.ToNode);
			const bool bFromPoly = IsPolyClass(FromC);
			const bool bToPoly = (ToC == EClass::PerVoice && Clones.count(L.ToNode) > 0);

			// Mono producer → broadcast: dest.R aliases dest.L (same buffer
			// pointer). Stereo producer → paired: dest.L = src.L, dest.R = src.R
			// so the two streams stay separate. The producer's IsOutputStereo
			// flag selects which.
			auto WireMonoLink = [&](INode* Source, uint32_t SrcPort, INode* Dest, uint32_t DestPort)
			{
				const bool bStereoSrc = Source->IsOutputStereo(SrcPort);
				float* SrcL = Source->GetOutputBuffer(SrcPort, 0);
				float* SrcR = bStereoSrc ? Source->GetOutputBuffer(SrcPort, 1) : SrcL;
				Dest->SetInputBuffer(DestPort, SrcL, 0);
				Dest->SetInputBuffer(DestPort, SrcR, 1);
			};

			if (!bFromPoly && !bToPoly)
			{
				// Mono → Mono.
				WireMonoLink(Nodes.at(L.FromNode).Node.get(), L.FromPort,
					Nodes.at(L.ToNode).Node.get(), L.ToPort);
			}
			else if (!bFromPoly && bToPoly)
			{
				// Mono → PerVoice: broadcast.
				INode* Source = Nodes.at(L.FromNode).Node.get();
				for (auto& Clone : Clones.at(L.ToNode))
				{
					WireMonoLink(Source, L.FromPort, Clone.get(), L.ToPort);
				}
			}
			else if (bFromPoly && bToPoly)
			{
				// PerVoice / VoiceAlloc → PerVoice: paired voice-i to voice-i.
				for (size_t V = 0; V < NumVoices; ++V)
				{
					float* VBuf = GetVoiceSourceBuffer(L.FromNode, L.FromPort, V);
					Clones.at(L.ToNode)[V]->SetInputBuffer(L.ToPort, VBuf, 0);
					Clones.at(L.ToNode)[V]->SetInputBuffer(L.ToPort, VBuf, 1);
				}
			}
			else
			{
				// PerVoice / VoiceAlloc → Mono Audio: route through the mixer.
				auto Key = std::make_pair(L.FromNode, L.FromPort);
				auto It = Mixers.find(Key);
				if (It != Mixers.end())
				{
					float* MixBuf = It->second->GetOutputBuffer(0);
					Nodes.at(L.ToNode).Node->SetInputBuffer(L.ToPort, MixBuf, 0);
					Nodes.at(L.ToNode).Node->SetInputBuffer(L.ToPort, MixBuf, 1);
				}
			}
		}

		Graph->OrderedNodes = std::move(OrderedNodes);
		Graph->OutputNode = OutputRec->Node;

		// Build NodeById + Allocators. NodeById maps original ids only — clones
		// don't get their own ids; SetParam fan-out to per-voice clones is a
		// follow-on (see CLAUDE.md). For now, SetParam on a per-voice node
		// updates the original (which isn't in OrderedNodes for per-voice nodes).
		Graph->NodeById.reserve(Order.size());
		for (FNodeId Id : Order)
		{
			FAudioGraph::FNodeEntry Entry;
			Entry.Primary = Nodes.at(Id).Node.get();
			if (auto It = Clones.find(Id); It != Clones.end())
			{
				Entry.Voices.reserve(It->second.size());
				for (auto& Clone : It->second)
				{
					Entry.Voices.push_back(Clone.get());
				}
			}
			Graph->NodeById.emplace(Id, std::move(Entry));
			if (auto* Alloc = dynamic_cast<FVoiceAllocator*>(Nodes.at(Id).Node.get()))
			{
				Graph->Allocators.push_back(Alloc);
			}
			if (auto* Cc = dynamic_cast<FMidiCC*>(Nodes.at(Id).Node.get()))
			{
				Graph->MidiCcNodes.push_back(Cc);
			}
		}

		return Graph;
	}

	void FAudioGraph::Process(const FProcessContext& Ctx)
	{
		for (auto& Node : OrderedNodes)
		{
			Node->Process(Ctx);
		}
	}

	void FAudioGraph::DrainCommands(FAudioCommandRing& Ring)
	{
		FAudioCommand Cmd;
		while (Ring.Pop(Cmd))
		{
			switch (Cmd.Type)
			{
				case EAudioCommand::SetParam:
				{
					auto It = NodeById.find(Cmd.NodeId);
					if (It != NodeById.end())
					{
						It->second.Primary->SetParamValue(Cmd.ParamIndex, Cmd.Value);
						// Fan out to per-voice clones so slider drags on a
						// per-voice node take audible effect on all voices.
						for (INode* Voice : It->second.Voices)
						{
							Voice->SetParamValue(Cmd.ParamIndex, Cmd.Value);
						}
					}
					break;
				}
				case EAudioCommand::NoteOn:
				{
					const uint8_t Note = static_cast<uint8_t>(Cmd.ParamIndex);
					for (FVoiceAllocator* Alloc : Allocators)
					{
						Alloc->HandleNoteOn(Note, Cmd.Value);
					}
					break;
				}
				case EAudioCommand::NoteOff:
				{
					const uint8_t Note = static_cast<uint8_t>(Cmd.ParamIndex);
					for (FVoiceAllocator* Alloc : Allocators)
					{
						Alloc->HandleNoteOff(Note);
					}
					break;
				}
			}
		}
	}
}
