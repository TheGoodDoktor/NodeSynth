#include "io/SubgraphSerializer.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "dsp/internal/SubgraphBoundary.h"
#include "io/GraphJson.h"

namespace NodeSynth
{
	namespace
	{
		using nlohmann::json;

		const char* PortTypeToString(EPortType Type)
		{
			return Type == EPortType::Control ? "control" : "audio";
		}

		// Anything that isn't "control" reads back as Audio — the default port
		// type and the safe fallback for a malformed / missing field.
		EPortType PortTypeFromString(const std::string& S)
		{
			return S == "control" ? EPortType::Control : EPortType::Audio;
		}

		json SerializePins(const std::vector<FSubgraphPin>& Pins)
		{
			json Arr = json::array();
			for (const FSubgraphPin& Pin : Pins)
			{
				json P;
				P["name"] = Pin.Name;
				P["type"] = PortTypeToString(Pin.Type);
				if (!Pin.Description.empty())
				{
					P["description"] = Pin.Description;
				}
				Arr.push_back(std::move(P));
			}
			return Arr;
		}

		std::vector<FSubgraphPin> DeserializePins(const json& Arr)
		{
			std::vector<FSubgraphPin> Pins;
			if (!Arr.is_array())
			{
				return Pins;
			}
			for (const json& P : Arr)
			{
				FSubgraphPin Pin;
				Pin.Name = P.value("name", std::string{});
				Pin.Type = PortTypeFromString(P.value("type", std::string{ "audio" }));
				Pin.Description = P.value("description", std::string{});
				Pins.push_back(std::move(Pin));
			}
			return Pins;
		}
	}

	json SerializeSubgraphDefinition(const FSubgraphDefinition& Def)
	{
		json Obj;
		Obj["name"] = Def.Name;
		Obj["input_pins"] = SerializePins(Def.InputPins);
		Obj["output_pins"] = SerializePins(Def.OutputPins);

		json Nodes = json::array();
		for (const auto& [Id, Rec] : Def.InternalGraph.GetNodes())
		{
			Nodes.push_back(GraphJson::SerializeNode(Rec));
		}
		Obj["nodes"] = std::move(Nodes);

		json Links = json::array();
		for (const FLink& L : Def.InternalGraph.GetLinks())
		{
			Links.push_back(GraphJson::SerializeLink(L));
		}
		Obj["links"] = std::move(Links);

		return Obj;
	}

	std::optional<FSubgraphDefinition> DeserializeSubgraphDefinition(const json& Obj)
	{
		if (!Obj.is_object())
		{
			std::fprintf(stderr, "DeserializeSubgraphDefinition: not a JSON object\n");
			return std::nullopt;
		}

		FSubgraphDefinition Def;
		Def.Name = Obj.value("name", std::string{});
		if (Def.Name.empty())
		{
			std::fprintf(stderr, "DeserializeSubgraphDefinition: missing or empty 'name'\n");
			return std::nullopt;
		}

		if (Obj.contains("input_pins"))
		{
			Def.InputPins = DeserializePins(Obj["input_pins"]);
		}
		if (Obj.contains("output_pins"))
		{
			Def.OutputPins = DeserializePins(Obj["output_pins"]);
		}

		// Internal graph carries no audio-command replay — params live on the
		// node objects and get copied into the per-instance clones at Compile
		// expansion time (SG.2), so there's nothing to push onto a ring here.
		if (Obj.contains("nodes"))
		{
			GraphJson::DeserializeNodes(Obj["nodes"], Def.InternalGraph, nullptr);
		}
		// Boundary nodes load without ports; push the declared pins onto them
		// before deserializing links, so boundary-incident links pass the port
		// type / range checks in FGraphModel::AddLink.
		SyncSubgraphBoundaries(Def);
		if (Obj.contains("links"))
		{
			GraphJson::DeserializeLinks(Obj["links"], Def.InternalGraph);
		}

		return Def;
	}

	bool SaveSubgraph(const FSubgraphDefinition& Def, const std::filesystem::path& Path)
	{
		try
		{
			std::ofstream Out(Path);
			if (!Out)
			{
				std::fprintf(stderr, "SaveSubgraph: could not open '%s' for writing\n",
					Path.string().c_str());
				return false;
			}
			Out << SerializeSubgraphDefinition(Def).dump(2);
		}
		catch (const std::exception& E)
		{
			std::fprintf(stderr, "SaveSubgraph: %s\n", E.what());
			return false;
		}
		return true;
	}

	std::optional<FSubgraphDefinition> LoadSubgraph(const std::filesystem::path& Path)
	{
		json Obj;
		try
		{
			std::ifstream In(Path);
			if (!In)
			{
				std::fprintf(stderr, "LoadSubgraph: could not open '%s'\n", Path.string().c_str());
				return std::nullopt;
			}
			In >> Obj;
		}
		catch (const std::exception& E)
		{
			std::fprintf(stderr, "LoadSubgraph: parse error in '%s': %s\n",
				Path.string().c_str(), E.what());
			return std::nullopt;
		}

		return DeserializeSubgraphDefinition(Obj);
	}
}
