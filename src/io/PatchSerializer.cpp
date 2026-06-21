#include "io/PatchSerializer.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "io/GraphJson.h"

namespace NodeSynth
{
	namespace
	{
		using nlohmann::json;
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
			Nodes.push_back(GraphJson::SerializeNode(Rec));
		}
		Root["nodes"] = std::move(Nodes);

		json Links = json::array();
		for (const FLink& L : Model.GetLinks())
		{
			Links.push_back(GraphJson::SerializeLink(L));
		}
		Root["links"] = std::move(Links);

		// MIDI mappings — only emit the array if there are any, so older
		// patches stay diff-clean.
		if (!Model.GetMidiMappings().empty())
		{
			json Mappings = json::array();
			for (const FMidiMapping& M : Model.GetMidiMappings())
			{
				json J;
				J["channel"] = static_cast<int>(M.Channel);
				J["cc"] = static_cast<int>(M.Cc);
				J["node_id"] = static_cast<uint64_t>(M.NodeId);
				J["param_index"] = M.ParamIndex;
				Mappings.push_back(std::move(J));
			}
			Root["midi_mappings"] = std::move(Mappings);
		}

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

		// -- Nodes & links -------------------------------------------------------
		// Shared with the subgraph serializer — same "nodes" / "links" shapes.
		if (Root.contains("nodes"))
		{
			GraphJson::DeserializeNodes(Root["nodes"], Result.Model, &Result.InitialParams);
		}
		if (Root.contains("links"))
		{
			GraphJson::DeserializeLinks(Root["links"], Result.Model);
		}

		// -- MIDI mappings --------------------------------------------------------
		// Optional, additive — older patches without this key load with no
		// mappings (back-compat). Stale mappings (target node deleted before
		// save somehow) silently no-op via FindNode failure at apply time.
		if (Root.contains("midi_mappings") && Root["midi_mappings"].is_array())
		{
			for (const json& J : Root["midi_mappings"])
			{
				FMidiMapping M;
				M.Channel = static_cast<uint8_t>(J.value("channel", 0));
				M.Cc = static_cast<uint8_t>(J.value("cc", 0));
				M.NodeId = J.value("node_id", uint64_t{ 0 });
				M.ParamIndex = J.value("param_index", uint32_t{ 0 });
				if (M.NodeId == 0) { continue; }
				Result.Model.AddMidiMapping(M);
			}
		}

		return Result;
	}
}
