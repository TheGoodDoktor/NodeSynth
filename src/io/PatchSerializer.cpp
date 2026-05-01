#include "io/PatchSerializer.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "ui/NodeRegistry.h"

namespace NodeSynth
{
	namespace
	{
		using nlohmann::json;

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
				Params[Infos[I].Name] = Rec.Node->GetParamValue(I);
			}
			N["params"] = std::move(Params);
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

	bool SavePatch(const FGraphModel& Model, const std::filesystem::path& Path)
	{
		json Root;
		Root["version"] = PatchSchemaVersion;

		// Metadata: only emit fields that the user has set (or that we know
		// because we just saved). Keeps default-empty patches diff-clean.
		const FPatchMetadata& Meta = Model.GetMetadata();
		json MetaJson = json::object();
		if (!Meta.Name.empty())   { MetaJson["name"] = Meta.Name; }
		if (!Meta.Author.empty()) { MetaJson["author"] = Meta.Author; }
		if (!Meta.Notes.empty())  { MetaJson["notes"] = Meta.Notes; }
		MetaJson["bpm"] = Meta.Bpm;
		if (Meta.SampleRateHint > 0.0)
		{
			MetaJson["sample_rate_hint"] = Meta.SampleRateHint;
		}
		if (!MetaJson.empty())
		{
			Root["metadata"] = std::move(MetaJson);
		}

		json Nodes = json::array();
		for (const auto& [Id, Rec] : Model.GetNodes())
		{
			Nodes.push_back(SerializeNode(Rec));
		}
		Root["nodes"] = std::move(Nodes);

		json Links = json::array();
		for (const FLink& L : Model.GetLinks())
		{
			Links.push_back(SerializeLink(L));
		}
		Root["links"] = std::move(Links);

		try
		{
			std::ofstream Out(Path);
			if (!Out)
			{
				std::fprintf(stderr, "SavePatch: could not open '%s' for writing\n",
					Path.string().c_str());
				return false;
			}
			Out << Root.dump(2);
		}
		catch (const std::exception& E)
		{
			std::fprintf(stderr, "SavePatch: %s\n", E.what());
			return false;
		}
		return true;
	}

	std::optional<FLoadedPatch> LoadPatch(const std::filesystem::path& Path)
	{
		json Root;
		try
		{
			std::ifstream In(Path);
			if (!In)
			{
				std::fprintf(stderr, "LoadPatch: could not open '%s'\n",
					Path.string().c_str());
				return std::nullopt;
			}
			In >> Root;
		}
		catch (const std::exception& E)
		{
			std::fprintf(stderr, "LoadPatch: parse error in '%s': %s\n",
				Path.string().c_str(), E.what());
			return std::nullopt;
		}

		const int32_t FileVersion = Root.value("version", -1);
		if (FileVersion != PatchSchemaVersion)
		{
			std::fprintf(stderr, "LoadPatch: schema version mismatch (file %d, expected %d)\n",
				FileVersion, PatchSchemaVersion);
			return std::nullopt;
		}

		FLoadedPatch Result;

		// -- Metadata ------------------------------------------------------------
		if (Root.contains("metadata") && Root["metadata"].is_object())
		{
			const json& M = Root["metadata"];
			FPatchMetadata& Meta = Result.Model.GetMetadata();
			Meta.Name = M.value("name", std::string{});
			Meta.Author = M.value("author", std::string{});
			Meta.Notes = M.value("notes", std::string{});
			Meta.Bpm = M.value("bpm", 120.0f);
			Meta.SampleRateHint = M.value("sample_rate_hint", 0.0);
		}

		// -- Nodes ---------------------------------------------------------------
		if (Root.contains("nodes") && Root["nodes"].is_array())
		{
			for (const json& N : Root["nodes"])
			{
				const std::string TypeName = N.value("type", std::string{});
				const FNodeId Id = N.value("id", uint64_t{ 0 });
				if (TypeName.empty() || Id == 0)
				{
					std::fprintf(stderr, "LoadPatch: skipping node with missing id/type\n");
					continue;
				}

				std::shared_ptr<INode> Node = MakeNodeByTypeName(TypeName);
				if (!Node)
				{
					std::fprintf(stderr, "LoadPatch: unknown node type '%s' — skipping id %llu\n",
						TypeName.c_str(), static_cast<unsigned long long>(Id));
					continue;
				}

				const float X = N.value("x", 0.0f);
				const float Y = N.value("y", 0.0f);
				const FNodeId Added = Result.Model.AddNodeWithId(Id, Node, X, Y);
				if (Added == 0)
				{
					std::fprintf(stderr, "LoadPatch: duplicate id or rejected node id %llu (type '%s')\n",
						static_cast<unsigned long long>(Id), TypeName.c_str());
					continue;
				}

				// Per-voice flag (defaults to false on missing key for back-compat
				// with v1 files written before this field existed).
				if (N.value("per_voice", false))
				{
					if (!Result.Model.SetNodePerVoice(Id, true))
					{
						std::fprintf(stderr,
							"LoadPatch: per_voice rejected for id %llu (type '%s' is not cloneable)\n",
							static_cast<unsigned long long>(Id), TypeName.c_str());
					}
				}

				// Params — keyed by name, mapped to the node's current param index.
				if (N.contains("params") && N["params"].is_object())
				{
					for (const auto& [Name, Value] : N["params"].items())
					{
						const int32_t ParamIndex = FindParamIndex(*Node, Name);
						if (ParamIndex < 0)
						{
							std::fprintf(stderr, "LoadPatch: unknown param '%s' on '%s' — skipping\n",
								Name.c_str(), TypeName.c_str());
							continue;
						}
						const float V = Value.is_number()
							? static_cast<float>(Value.get<double>())
							: 0.0f;
						Node->SetParamValue(static_cast<uint32_t>(ParamIndex), V);
						Result.InitialParams.push_back(
							FAudioCommand::MakeSetParam(Id, static_cast<uint32_t>(ParamIndex), V));
					}
				}
			}
		}

		// -- Links ---------------------------------------------------------------
		if (Root.contains("links") && Root["links"].is_array())
		{
			for (const json& L : Root["links"])
			{
				const FNodeId FromNode = L.value("from_node", uint64_t{ 0 });
				const FNodeId ToNode = L.value("to_node", uint64_t{ 0 });
				const uint32_t FromPort = L.value("from_port", uint32_t{ 0 });
				const uint32_t ToPort = L.value("to_port", uint32_t{ 0 });
				if (FromNode == 0 || ToNode == 0)
				{
					continue;
				}
				const FLinkId Added = Result.Model.AddLink(FromNode, FromPort, ToNode, ToPort);
				if (Added == 0)
				{
					std::fprintf(stderr,
						"LoadPatch: link rejected (%llu:%u -> %llu:%u) — skipping\n",
						static_cast<unsigned long long>(FromNode), FromPort,
						static_cast<unsigned long long>(ToNode), ToPort);
				}
			}
		}

		return Result;
	}
}
