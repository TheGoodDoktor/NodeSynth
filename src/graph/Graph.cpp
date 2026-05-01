#include "graph/Graph.h"

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>

#include "dsp/MidiInput.h"
#include "dsp/VoiceAllocator.h"
#include "dsp/internal/VoiceMixer.h"
#include "graph/EditHistory.h"

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

	std::shared_ptr<FAudioGraph> FGraphModel::Compile(double SampleRate)
	{
		LastCompileError = FCompileError{};  // clear at the start of every Compile
		auto Graph = std::make_shared<FAudioGraph>();

		// -- Step 1: locate the output sink --------------------------------------
		FNodeRecord* OutputRec = nullptr;
		for (auto& [Id, Rec] : Nodes)
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
		}

		// Hand each FMidiInput the allocator list for direct in-block dispatch.
		for (FNodeId Id : Order)
		{
			if (auto* Midi = dynamic_cast<FMidiInput*>(Nodes.at(Id).Node.get()))
			{
				Midi->SetVoiceAllocators(Graph->Allocators);
			}
		}

		return Graph;
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
