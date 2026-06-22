#include "io/GraphJson.h"

#include <cstdio>
#include <string>

#include "dsp/Subgraph.h"
#include "graph/SubgraphDefinition.h"
#include "ui/NodeRegistry.h"

namespace NodeSynth::GraphJson
{
	namespace
	{
		using nlohmann::json;

		// Returns -1 if the param name isn't on the node.
		int32_t FindParamIndex(const INode& Node, const std::string& Name)
		{
			const auto Infos = Node.GetParamInfos();
			for (uint32_t I = 0; I < Infos.size(); ++I)
			{
				if (Infos[I].Name == Name)
				{
					return static_cast<int32_t>(I);
				}
			}
			return -1;
		}
	}

	json SerializeNode(const FNodeRecord& Rec)
	{
		json N;
		N["id"] = static_cast<uint64_t>(Rec.Id);
		N["type"] = Rec.Node->GetTypeName();
		N["x"] = Rec.PositionX;
		N["y"] = Rec.PositionY;
		// Only emit the flag when set so older files stay diff-clean.
		if (Rec.bPerVoice)
		{
			N["per_voice"] = true;
		}

		json Params = json::object();
		const auto Infos = Rec.Node->GetParamInfos();
		for (uint32_t I = 0; I < Infos.size(); ++I)
		{
			if (Infos[I].Kind == EParamKind::String)
			{
				Params[Infos[I].Name] = Rec.Node->GetParamString(I);
			}
			else
			{
				Params[Infos[I].Name] = Rec.Node->GetParamValue(I);
			}
		}
		N["params"] = std::move(Params);

		// Subgraph instances reference their definition by name; the definition
		// itself is serialized in the patch's "subgraphs" block.
		if (auto* Sub = dynamic_cast<FSubgraph*>(Rec.Node.get()))
		{
			if (Sub->GetDefinition())
			{
				N["subgraph_name"] = Sub->GetDefinition()->Name;
			}
		}
		return N;
	}

	json SerializeLink(const FLink& L)
	{
		json J;
		J["id"] = static_cast<uint64_t>(L.Id);
		J["from_node"] = static_cast<uint64_t>(L.FromNode);
		J["from_port"] = L.FromPort;
		J["to_node"] = static_cast<uint64_t>(L.ToNode);
		J["to_port"] = L.ToPort;
		return J;
	}

	void DeserializeNodes(const json& NodesArray, FGraphModel& Model,
		std::vector<FAudioCommand>* OutInitialParams)
	{
		if (!NodesArray.is_array())
		{
			return;
		}

		for (const json& N : NodesArray)
		{
			const std::string TypeName = N.value("type", std::string{});
			const FNodeId Id = N.value("id", uint64_t{ 0 });
			if (TypeName.empty() || Id == 0)
			{
				std::fprintf(stderr, "GraphJson: skipping node with missing id/type\n");
				continue;
			}

			// Deprecated node types removed when note input moved out of the
			// graph (MIDI device + on-screen keyboard are now project-level).
			// Patches saved before that change still mention them; skip with a
			// clear warning so the rest of the graph loads.
			if (TypeName == "MIDI" || TypeName == "VirtualKbd")
			{
				std::fprintf(stderr,
					"GraphJson: deprecated node type '%s' (id %llu) — note input is now project-level; skipping. "
					"Use the on-screen keyboard panel and the MIDI Device combo at the top of it.\n",
					TypeName.c_str(), static_cast<unsigned long long>(Id));
				continue;
			}

			std::shared_ptr<INode> Node = MakeNodeByTypeName(TypeName);
			if (!Node)
			{
				std::fprintf(stderr, "GraphJson: unknown node type '%s' — skipping id %llu\n",
					TypeName.c_str(), static_cast<unsigned long long>(Id));
				continue;
			}

			const float X = N.value("x", 0.0f);
			const float Y = N.value("y", 0.0f);
			const FNodeId Added = Model.AddNodeWithId(Id, Node, X, Y);
			if (Added == 0)
			{
				std::fprintf(stderr, "GraphJson: duplicate id or rejected node id %llu (type '%s')\n",
					static_cast<unsigned long long>(Id), TypeName.c_str());
				continue;
			}

			// Per-voice flag (defaults to false on missing key for back-compat
			// with v1 files written before this field existed).
			if (N.value("per_voice", false))
			{
				if (!Model.SetNodePerVoice(Id, true))
				{
					std::fprintf(stderr,
						"GraphJson: per_voice rejected for id %llu (type '%s' is not cloneable)\n",
						static_cast<unsigned long long>(Id), TypeName.c_str());
				}
			}

			// Params — keyed by name, mapped to the node's current param index.
			if (N.contains("params") && N["params"].is_object())
			{
				const auto Infos = Node->GetParamInfos();
				for (const auto& [Name, Value] : N["params"].items())
				{
					const int32_t ParamIndex = FindParamIndex(*Node, Name);
					if (ParamIndex < 0)
					{
						std::fprintf(stderr, "GraphJson: unknown param '%s' on '%s' — skipping\n",
							Name.c_str(), TypeName.c_str());
						continue;
					}
					const EParamKind Kind = Infos[ParamIndex].Kind;
					if (Kind == EParamKind::String)
					{
						const std::string S = Value.is_string()
							? Value.get<std::string>()
							: std::string{};
						Node->SetParamString(static_cast<uint32_t>(ParamIndex), S);
						// String params don't roundtrip through the audio command
						// queue — they're UI-thread-only and not RT-safe (file I/O
						// on SetParamString).
					}
					else
					{
						const float V = Value.is_number()
							? static_cast<float>(Value.get<double>())
							: 0.0f;
						Node->SetParamValue(static_cast<uint32_t>(ParamIndex), V);
						if (OutInitialParams != nullptr)
						{
							OutInitialParams->push_back(
								FAudioCommand::MakeSetParam(Id, static_cast<uint32_t>(ParamIndex), V));
						}
					}
				}
			}
		}
	}

	void DeserializeLinks(const json& LinksArray, FGraphModel& Model)
	{
		if (!LinksArray.is_array())
		{
			return;
		}

		for (const json& L : LinksArray)
		{
			const FNodeId FromNode = L.value("from_node", uint64_t{ 0 });
			const FNodeId ToNode = L.value("to_node", uint64_t{ 0 });
			const uint32_t FromPort = L.value("from_port", uint32_t{ 0 });
			const uint32_t ToPort = L.value("to_port", uint32_t{ 0 });
			if (FromNode == 0 || ToNode == 0)
			{
				continue;
			}
			const FLinkId Added = Model.AddLink(FromNode, FromPort, ToNode, ToPort);
			if (Added == 0)
			{
				std::fprintf(stderr,
					"GraphJson: link rejected (%llu:%u -> %llu:%u) — skipping\n",
					static_cast<unsigned long long>(FromNode), FromPort,
					static_cast<unsigned long long>(ToNode), ToPort);
			}
		}
	}

	void BindSubgraphInstances(FGraphModel& Model, const json& NodesArray,
		const std::unordered_map<std::string, std::shared_ptr<FSubgraphDefinition>>& Defs)
	{
		if (!NodesArray.is_array())
		{
			return;
		}
		for (const json& N : NodesArray)
		{
			if (!N.contains("subgraph_name"))
			{
				continue;
			}
			const FNodeId Id = N.value("id", uint64_t{ 0 });
			const std::string Name = N.value("subgraph_name", std::string{});
			FNodeRecord* Rec = Model.FindNode(Id);
			if (!Rec || !Rec->Node)
			{
				continue;
			}
			if (auto* Sub = dynamic_cast<FSubgraph*>(Rec->Node.get()))
			{
				auto It = Defs.find(Name);
				if (It != Defs.end())
				{
					Sub->SetDefinition(It->second);
				}
				else
				{
					std::fprintf(stderr,
						"GraphJson: subgraph instance id %llu references unknown definition '%s'\n",
						static_cast<unsigned long long>(Id), Name.c_str());
				}
			}
		}
	}
}
