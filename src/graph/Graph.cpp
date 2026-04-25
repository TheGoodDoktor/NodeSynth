#include "graph/Graph.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_set>

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
		Nodes.emplace(Id, FNodeRecord{ Id, std::move(Node), X, Y });
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
		Nodes.emplace(Id, FNodeRecord{ Id, std::move(Node), X, Y });
		if (Id >= NextNodeId)
		{
			NextNodeId = Id + 1;
		}
		return Id;
	}

	void FGraphModel::RemoveNode(FNodeId Id)
	{
		Links.erase(std::remove_if(Links.begin(), Links.end(),
			[Id](const FLink& L) { return L.FromNode == Id || L.ToNode == Id; }),
			Links.end());
		Nodes.erase(Id);
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
		It->second.bPerVoice = bPerVoice;
		return true;
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

		// An input port takes one connection at a time. Drop any existing.
		Links.erase(std::remove_if(Links.begin(), Links.end(),
			[ToNode, ToPort](const FLink& L) { return L.ToNode == ToNode && L.ToPort == ToPort; }),
			Links.end());

		if (WouldCreateCycle(Links, FromNode, ToNode))
		{
			return 0;
		}

		const FLinkId Id = NextLinkId++;
		Links.push_back(FLink{ Id, FromNode, FromPort, ToNode, ToPort });
		return Id;
	}

	void FGraphModel::RemoveLink(FLinkId Id)
	{
		Links.erase(std::remove_if(Links.begin(), Links.end(),
			[Id](const FLink& L) { return L.Id == Id; }),
			Links.end());
	}

	std::shared_ptr<FAudioGraph> FGraphModel::Compile(double SampleRate)
	{
		auto Graph = std::make_shared<FAudioGraph>();

		// Locate the output sink. Phase 1 supports one; extras are ignored.
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

		// Reverse DFS from the output, producing a producers-first order.
		std::unordered_set<FNodeId> Visited;
		std::vector<std::shared_ptr<INode>> Order;

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
			auto It = Nodes.find(Id);
			if (It != Nodes.end())
			{
				Order.push_back(It->second.Node);
			}
		};

		Visit(OutputRec->Id);

		// Clear all input pointers so disconnected inputs read null (nodes handle it).
		for (auto& Node : Order)
		{
			const uint32_t NumInputs = static_cast<uint32_t>(Node->GetInputPorts().size());
			for (uint32_t I = 0; I < NumInputs; ++I)
			{
				Node->SetInputBuffer(I, nullptr);
			}
			Node->Prepare(SampleRate);
		}

		// Route: for each link, resolve the upstream output buffer and hand it to the downstream input.
		for (const FLink& L : Links)
		{
			auto FromIt = Nodes.find(L.FromNode);
			auto ToIt = Nodes.find(L.ToNode);
			if (FromIt == Nodes.end() || ToIt == Nodes.end())
			{
				continue;
			}
			// Only connect if both nodes are part of the compiled subgraph
			// (unreachable branches are skipped entirely).
			if (Visited.count(L.FromNode) == 0 || Visited.count(L.ToNode) == 0)
			{
				continue;
			}
			float* UpstreamBuffer = FromIt->second.Node->GetOutputBuffer(L.FromPort);
			ToIt->second.Node->SetInputBuffer(L.ToPort, UpstreamBuffer);
		}

		Graph->OrderedNodes = std::move(Order);
		Graph->OutputNode = OutputRec->Node;

		// Build the id-to-node lookup so the audio-thread command drain can find
		// nodes by FNodeId without scanning OrderedNodes.
		Graph->NodeById.reserve(Graph->OrderedNodes.size());
		for (const auto& [Id, Rec] : Nodes)
		{
			if (Visited.count(Id) > 0)
			{
				Graph->NodeById.emplace(Id, Rec.Node.get());
			}
		}

		return Graph;
	}

	void FAudioGraph::DrainCommands(FAudioCommandRing& Ring)
	{
		FAudioCommand Cmd;
		while (Ring.Pop(Cmd))
		{
			auto It = NodeById.find(Cmd.NodeId);
			if (It == NodeById.end())
			{
				continue;  // node not in this snapshot — UI thread already wrote the atomic
			}
			switch (Cmd.Type)
			{
				case EAudioCommand::SetParam:
					It->second->SetParamValue(Cmd.ParamIndex, Cmd.Value);
					break;
			}
		}
	}
}
