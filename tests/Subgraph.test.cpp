#include "io/SubgraphSerializer.h"

#include "dsp/Oscillator.h"
#include "dsp/Svf.h"
#include "graph/SubgraphDefinition.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <random>

using Catch::Matchers::WithinAbs;
using NodeSynth::DeserializeSubgraphDefinition;
using NodeSynth::EPortType;
using NodeSynth::FNodeId;
using NodeSynth::FOscillator;
using NodeSynth::FSubgraphDefinition;
using NodeSynth::FSubgraphPin;
using NodeSynth::FSvf;
using NodeSynth::LoadSubgraph;
using NodeSynth::SaveSubgraph;
using NodeSynth::SerializeSubgraphDefinition;

namespace
{
	std::filesystem::path TempSubgraphPath()
	{
		std::random_device Rd;
		std::mt19937_64 Rng(Rd());
		std::filesystem::path Dir = std::filesystem::temp_directory_path() / "nodesynth_tests";
		std::filesystem::create_directories(Dir);
		return Dir / ("subgraph_" + std::to_string(Rng()) + ".nspg");
	}

	// A "Stereo Filter"-shaped definition: Osc -> SVF internally, with one
	// Audio input pin, two Control input pins, and one Audio output pin.
	FSubgraphDefinition MakeSampleDefinition(FNodeId& OutOscId, FNodeId& OutSvfId)
	{
		FSubgraphDefinition Def;
		Def.Name = "StereoFilter";
		Def.InputPins = {
			{ "Audio", EPortType::Audio, "Signal in" },
			{ "Cutoff", EPortType::Control, "Filter cutoff" },
			{ "Resonance", EPortType::Control, "" },
		};
		Def.OutputPins = {
			{ "Out", EPortType::Audio, "Filtered signal" },
		};

		auto Osc = std::make_shared<FOscillator>();
		auto Filter = std::make_shared<FSvf>();
		Osc->SetParamValue(FOscillator::Param_Frequency, 330.0f);
		Osc->SetParamValue(FOscillator::Param_Amplitude, 0.5f);

		OutOscId = Def.InternalGraph.AddNode(Osc, 40.0f, 80.0f);
		OutSvfId = Def.InternalGraph.AddNode(Filter, 280.0f, 80.0f);
		Def.InternalGraph.AddLink(OutOscId, 0, OutSvfId, 0);
		return Def;
	}
}

TEST_CASE("Subgraph definition: JSON round-trip preserves pins, nodes, links, params", "[subgraph]")
{
	FNodeId OscId = 0;
	FNodeId SvfId = 0;
	FSubgraphDefinition Original = MakeSampleDefinition(OscId, SvfId);

	const auto Obj = SerializeSubgraphDefinition(Original);
	auto Loaded = DeserializeSubgraphDefinition(Obj);
	REQUIRE(Loaded.has_value());

	REQUIRE(Loaded->Name == "StereoFilter");

	// Pins — order, names, types, and descriptions all preserved.
	REQUIRE(Loaded->InputPins.size() == 3);
	REQUIRE(Loaded->InputPins[0].Name == "Audio");
	REQUIRE(Loaded->InputPins[0].Type == EPortType::Audio);
	REQUIRE(Loaded->InputPins[0].Description == "Signal in");
	REQUIRE(Loaded->InputPins[1].Name == "Cutoff");
	REQUIRE(Loaded->InputPins[1].Type == EPortType::Control);
	REQUIRE(Loaded->InputPins[2].Name == "Resonance");
	REQUIRE(Loaded->InputPins[2].Description.empty());

	REQUIRE(Loaded->OutputPins.size() == 1);
	REQUIRE(Loaded->OutputPins[0].Name == "Out");
	REQUIRE(Loaded->OutputPins[0].Type == EPortType::Audio);

	// Internal graph — node ids preserved (AddNodeWithId on load), link intact.
	const auto& Nodes = Loaded->InternalGraph.GetNodes();
	REQUIRE(Nodes.size() == 2);
	REQUIRE(Nodes.count(OscId) == 1);
	REQUIRE(Nodes.count(SvfId) == 1);

	const auto& Links = Loaded->InternalGraph.GetLinks();
	REQUIRE(Links.size() == 1);
	REQUIRE(Links[0].FromNode == OscId);
	REQUIRE(Links[0].ToNode == SvfId);

	// Params survive the round-trip.
	const auto* OscRec = Nodes.at(OscId).Node.get();
	REQUIRE_THAT(OscRec->GetParamValue(FOscillator::Param_Frequency), WithinAbs(330.0f, 1e-4f));
	REQUIRE_THAT(OscRec->GetParamValue(FOscillator::Param_Amplitude), WithinAbs(0.5f, 1e-4f));
}

TEST_CASE("Subgraph definition: .nspg file save/load round-trip", "[subgraph]")
{
	FNodeId OscId = 0;
	FNodeId SvfId = 0;
	FSubgraphDefinition Original = MakeSampleDefinition(OscId, SvfId);

	const auto Path = TempSubgraphPath();
	REQUIRE(SaveSubgraph(Original, Path));

	auto Loaded = LoadSubgraph(Path);
	REQUIRE(Loaded.has_value());
	REQUIRE(Loaded->Name == "StereoFilter");
	REQUIRE(Loaded->InputPins.size() == 3);
	REQUIRE(Loaded->OutputPins.size() == 1);
	REQUIRE(Loaded->InternalGraph.GetNodes().size() == 2);
	REQUIRE(Loaded->InternalGraph.GetLinks().size() == 1);

	std::filesystem::remove(Path);
}

TEST_CASE("Subgraph definition: a definition with no name is rejected", "[subgraph]")
{
	FSubgraphDefinition Def;  // empty name
	const auto Obj = SerializeSubgraphDefinition(Def);
	REQUIRE_FALSE(DeserializeSubgraphDefinition(Obj).has_value());
}

TEST_CASE("Subgraph definition: empty internal graph round-trips", "[subgraph]")
{
	FSubgraphDefinition Def;
	Def.Name = "Empty";
	Def.InputPins = { { "In", EPortType::Audio, "" } };
	Def.OutputPins = { { "Out", EPortType::Audio, "" } };

	auto Loaded = DeserializeSubgraphDefinition(SerializeSubgraphDefinition(Def));
	REQUIRE(Loaded.has_value());
	REQUIRE(Loaded->Name == "Empty");
	REQUIRE(Loaded->InputPins.size() == 1);
	REQUIRE(Loaded->OutputPins.size() == 1);
	REQUIRE(Loaded->InternalGraph.GetNodes().empty());
	REQUIRE(Loaded->InternalGraph.GetLinks().empty());
}
