#include "graph/SubgraphOps.h"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "dsp/Subgraph.h"
#include "dsp/internal/SubgraphBoundary.h"
#include "graph/Graph.h"
#include "graph/SubgraphDefinition.h"

namespace NodeSynth
{
	namespace
	{
		bool IsBoundary(const INode& Node)
		{
			const std::string T = Node.GetTypeName();
			return T == "_SubgraphInputs" || T == "_SubgraphOutputs";
		}
	}

	FNodeId GroupNodesIntoSubgraph(FGraphModel& Patch, const std::vector<FNodeId>& Selected)
	{
		// Filter: skip the patch sink, the voice allocator, boundary nodes, and
		// anything that can't be cloned.
		std::unordered_set<FNodeId> Sel;
		for (const FNodeId Id : Selected)
		{
			FNodeRecord* Rec = Patch.FindNode(Id);
			if (!Rec || !Rec->Node) { continue; }
			const std::string T = Rec->Node->GetTypeName();
			if (T == "Output" || T == "VoiceAllocator" || IsBoundary(*Rec->Node)) { continue; }
			if (Rec->Node->Clone() == nullptr) { continue; }
			Sel.insert(Id);
		}
		if (Sel.empty())
		{
			return 0;
		}

		float Cx = 0.0f;
		float Cy = 0.0f;
		for (const FNodeId Id : Sel)
		{
			const FNodeRecord* Rec = Patch.FindNode(Id);
			Cx += Rec->PositionX;
			Cy += Rec->PositionY;
		}
		Cx /= static_cast<float>(Sel.size());
		Cy /= static_cast<float>(Sel.size());

		auto Def = std::make_shared<FSubgraphDefinition>();
		std::string Name;
		int32_t Serial = 1;
		do
		{
			Name = "Group " + std::to_string(Serial++);
		}
		while (Patch.FindSubgraphDefinition(Name));
		Def->Name = Name;

		auto Inputs = std::make_shared<Internal::FSubgraphInputs>();
		auto Outputs = std::make_shared<Internal::FSubgraphOutputs>();
		const FNodeId InId = Def->InternalGraph.AddNode(Inputs, -320.0f, 0.0f);
		const FNodeId OutId = Def->InternalGraph.AddNode(Outputs, 320.0f, 0.0f);

		std::unordered_map<FNodeId, FNodeId> IdMap;
		for (const FNodeId Id : Sel)
		{
			FNodeRecord* Rec = Patch.FindNode(Id);
			auto Clone = Rec->Node->Clone();
			if (!Clone) { continue; }
			Clone->MasterMirror = nullptr;
			IdMap[Id] = Def->InternalGraph.AddNode(Clone, Rec->PositionX - Cx, Rec->PositionY - Cy);
		}

		// Classify links; allocate pins. Pins must all exist before boundary
		// links are added (SyncSubgraphBoundaries gives the boundary nodes their
		// ports), so record the work and apply it after syncing.
		struct FPendingLink { FNodeId From; uint32_t FromPort; FNodeId To; uint32_t ToPort; };
		std::vector<FPendingLink> InternalLinks;
		struct FExtIn { FNodeId Producer; uint32_t ProducerPort; uint32_t Pin; };
		struct FExtOut { uint32_t Pin; FNodeId Consumer; uint32_t ConsumerPort; };
		std::vector<FExtIn> ExtIns;
		std::vector<FExtOut> ExtOuts;
		std::map<std::pair<FNodeId, uint32_t>, uint32_t> OutPinByPort;

		for (const FLink& L : Patch.GetLinks())
		{
			const bool bFromSel = Sel.count(L.FromNode) > 0;
			const bool bToSel = Sel.count(L.ToNode) > 0;
			if (bFromSel && bToSel)
			{
				InternalLinks.push_back({ IdMap[L.FromNode], L.FromPort, IdMap[L.ToNode], L.ToPort });
			}
			else if (bToSel)
			{
				FNodeRecord* Rec = Patch.FindNode(L.ToNode);
				const auto Ports = Rec->Node->GetInputPorts();
				const EPortType Type = (L.ToPort < Ports.size()) ? Ports[L.ToPort].Type : EPortType::Audio;
				const std::string PinName = (L.ToPort < Ports.size()) ? Ports[L.ToPort].Name : "In";
				const uint32_t Pin = static_cast<uint32_t>(Def->InputPins.size());
				Def->InputPins.push_back({ PinName, Type, "" });
				InternalLinks.push_back({ InId, Pin, IdMap[L.ToNode], L.ToPort });
				ExtIns.push_back({ L.FromNode, L.FromPort, Pin });
			}
			else if (bFromSel)
			{
				const auto Key = std::make_pair(L.FromNode, L.FromPort);
				auto It = OutPinByPort.find(Key);
				uint32_t Pin;
				if (It == OutPinByPort.end())
				{
					FNodeRecord* Rec = Patch.FindNode(L.FromNode);
					const auto Ports = Rec->Node->GetOutputPorts();
					const EPortType Type = (L.FromPort < Ports.size()) ? Ports[L.FromPort].Type : EPortType::Audio;
					const std::string PinName = (L.FromPort < Ports.size()) ? Ports[L.FromPort].Name : "Out";
					Pin = static_cast<uint32_t>(Def->OutputPins.size());
					Def->OutputPins.push_back({ PinName, Type, "" });
					OutPinByPort[Key] = Pin;
					InternalLinks.push_back({ IdMap[L.FromNode], L.FromPort, OutId, Pin });
				}
				else
				{
					Pin = It->second;
				}
				ExtOuts.push_back({ Pin, L.ToNode, L.ToPort });
			}
		}

		SyncSubgraphBoundaries(*Def);
		for (const FPendingLink& L : InternalLinks)
		{
			Def->InternalGraph.AddLink(L.From, L.FromPort, L.To, L.ToPort);
		}

		// Replace the selected nodes with a single instance.
		for (const FNodeId Id : Sel)
		{
			Patch.RemoveNode(Id);  // also drops the boundary-crossing links
		}
		auto Stored = Patch.AddSubgraphDefinition(Def);
		auto Instance = std::make_shared<FSubgraph>();
		Instance->SetDefinition(Stored);
		const FNodeId InstId = Patch.AddNode(Instance, Cx, Cy);
		for (const FExtIn& E : ExtIns)
		{
			Patch.AddLink(E.Producer, E.ProducerPort, InstId, E.Pin);
		}
		for (const FExtOut& E : ExtOuts)
		{
			Patch.AddLink(InstId, E.Pin, E.Consumer, E.ConsumerPort);
		}
		return InstId;
	}
}
