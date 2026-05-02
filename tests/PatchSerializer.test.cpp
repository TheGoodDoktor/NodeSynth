#include "io/PatchSerializer.h"

#include "dsp/Adsr.h"
#include "dsp/Gain.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "graph/Graph.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <fstream>
#include <random>

using NodeSynth::FAdsr;
using NodeSynth::FAudioCommand;
using NodeSynth::FGain;
using NodeSynth::FGraphModel;
using NodeSynth::FLink;
using NodeSynth::FLoadedPatch;
using NodeSynth::FNodeId;
using NodeSynth::FOscillator;
using NodeSynth::FOutput;
using NodeSynth::LoadPatch;
using NodeSynth::SavePatch;

namespace
{
	std::filesystem::path TempPatchPath()
	{
		// Unique temp filename per test invocation so parallel test runs don't
		// stomp on each other.
		std::random_device Rd;
		std::mt19937_64 Rng(Rd());
		std::filesystem::path Dir = std::filesystem::temp_directory_path() / "nodesynth_tests";
		std::filesystem::create_directories(Dir);
		return Dir / ("patch_" + std::to_string(Rng()) + ".json");
	}
}

TEST_CASE("Patch serializer: round-trip preserves nodes, params, and links", "[patch]")
{
	FGraphModel Original;
	auto Osc = std::make_shared<FOscillator>();
	auto GainN = std::make_shared<FGain>();
	auto Out = std::make_shared<FOutput>();
	const FNodeId OscId = Original.AddNode(Osc, 60.0f, 120.0f);
	const FNodeId GainId = Original.AddNode(GainN, 320.0f, 120.0f);
	const FNodeId OutId = Original.AddNode(Out, 580.0f, 120.0f);
	Original.AddLink(OscId, 0, GainId, 0);
	Original.AddLink(GainId, 0, OutId, 0);

	// Tweak some params so we can verify they persist.
	Osc->SetParamValue(FOscillator::Param_Frequency, 220.0f);
	Osc->SetParamValue(FOscillator::Param_Amplitude, 0.42f);
	GainN->SetParamValue(FGain::Param_Gain, 0.75f);

	const auto Path = TempPatchPath();
	REQUIRE(SavePatch(Original, Path));

	auto Loaded = LoadPatch(Path);
	REQUIRE(Loaded.has_value());

	const auto& Nodes = Loaded->Model.GetNodes();
	REQUIRE(Nodes.size() == 3);
	REQUIRE(Nodes.count(OscId) == 1);
	REQUIRE(Nodes.count(GainId) == 1);
	REQUIRE(Nodes.count(OutId) == 1);

	const auto& OscRec = Nodes.at(OscId);
	REQUIRE(std::string(OscRec.Node->GetTypeName()) == "Oscillator");
	REQUIRE(OscRec.PositionX == 60.0f);
	REQUIRE(OscRec.PositionY == 120.0f);
	REQUIRE_THAT(OscRec.Node->GetParamValue(FOscillator::Param_Frequency),
		Catch::Matchers::WithinAbs(220.0f, 1e-3f));
	REQUIRE_THAT(OscRec.Node->GetParamValue(FOscillator::Param_Amplitude),
		Catch::Matchers::WithinAbs(0.42f, 1e-3f));

	const auto& GainRec = Nodes.at(GainId);
	REQUIRE_THAT(GainRec.Node->GetParamValue(FGain::Param_Gain),
		Catch::Matchers::WithinAbs(0.75f, 1e-3f));

	const auto& Links = Loaded->Model.GetLinks();
	REQUIRE(Links.size() == 2);

	std::filesystem::remove(Path);
}

TEST_CASE("Patch serializer: MIDI mappings round-trip", "[patch][midi]")
{
	FGraphModel Original;
	auto GainN = std::make_shared<FGain>();
	auto Out = std::make_shared<FOutput>();
	const FNodeId GainId = Original.AddNode(GainN);
	const FNodeId OutId = Original.AddNode(Out);
	(void)OutId;
	Original.AddLink(GainId, 0, OutId, 0);

	NodeSynth::FMidiMapping M;
	M.Channel = 1;
	M.Cc = 16;
	M.NodeId = GainId;
	M.ParamIndex = 0;
	Original.AddMidiMapping(M);

	const auto Path = TempPatchPath();
	REQUIRE(SavePatch(Original, Path));

	auto Loaded = LoadPatch(Path);
	REQUIRE(Loaded.has_value());
	const auto& Mappings = Loaded->Model.GetMidiMappings();
	REQUIRE(Mappings.size() == 1);
	REQUIRE(Mappings[0].Channel == 1);
	REQUIRE(Mappings[0].Cc == 16);
	REQUIRE(Mappings[0].NodeId == GainId);
	REQUIRE(Mappings[0].ParamIndex == 0);
	std::filesystem::remove(Path);
}

TEST_CASE("Patch serializer: patch without midi_mappings field loads with empty list", "[patch][midi]")
{
	FGraphModel Original;
	auto GainN = std::make_shared<FGain>();
	auto Out = std::make_shared<FOutput>();
	Original.AddNode(GainN);
	Original.AddNode(Out);

	const auto Path = TempPatchPath();
	REQUIRE(SavePatch(Original, Path));

	auto Loaded = LoadPatch(Path);
	REQUIRE(Loaded.has_value());
	REQUIRE(Loaded->Model.GetMidiMappings().empty());
	std::filesystem::remove(Path);
}

TEST_CASE("Patch serializer: InitialParams mirror saved param values", "[patch]")
{
	FGraphModel Original;
	auto Adsr = std::make_shared<FAdsr>();
	auto Out = std::make_shared<FOutput>();
	const FNodeId AdsrId = Original.AddNode(Adsr, 0.0f, 0.0f);
	Original.AddNode(Out, 0.0f, 0.0f);
	Adsr->SetParamValue(FAdsr::Param_AttackMs, 12.5f);
	Adsr->SetParamValue(FAdsr::Param_Sustain, 0.6f);

	const auto Path = TempPatchPath();
	REQUIRE(SavePatch(Original, Path));
	auto Loaded = LoadPatch(Path);
	REQUIRE(Loaded.has_value());

	// At least one InitialParams entry should mention the ADSR's Attack value.
	bool bSawAttack = false;
	bool bSawSustain = false;
	for (const FAudioCommand& Cmd : Loaded->InitialParams)
	{
		if (Cmd.NodeId == AdsrId && Cmd.ParamIndex == FAdsr::Param_AttackMs)
		{
			REQUIRE_THAT(Cmd.Value, Catch::Matchers::WithinAbs(12.5f, 1e-3f));
			bSawAttack = true;
		}
		if (Cmd.NodeId == AdsrId && Cmd.ParamIndex == FAdsr::Param_Sustain)
		{
			REQUIRE_THAT(Cmd.Value, Catch::Matchers::WithinAbs(0.6f, 1e-3f));
			bSawSustain = true;
		}
	}
	REQUIRE(bSawAttack);
	REQUIRE(bSawSustain);

	std::filesystem::remove(Path);
}

TEST_CASE("Patch serializer: rejects schema version mismatch", "[patch]")
{
	const auto Path = TempPatchPath();
	{
		std::ofstream Out(Path);
		Out << R"({"version": 999, "nodes": [], "links": []})";
	}

	auto Loaded = LoadPatch(Path);
	REQUIRE_FALSE(Loaded.has_value());

	std::filesystem::remove(Path);
}

TEST_CASE("Patch serializer: nullopt when file is missing", "[patch]")
{
	const auto Path = std::filesystem::temp_directory_path()
		/ "nodesynth_tests" / "definitely_does_not_exist.json";
	auto Loaded = LoadPatch(Path);
	REQUIRE_FALSE(Loaded.has_value());
}

TEST_CASE("Patch serializer: drops the second Output (singleton enforcement)", "[patch]")
{
	const auto Path = TempPatchPath();
	{
		std::ofstream F(Path);
		F << R"({
			"version": 1,
			"nodes": [
				{"id": 1, "type": "Output", "x": 0, "y": 0, "params": {}},
				{"id": 2, "type": "Output", "x": 0, "y": 0, "params": {}}
			],
			"links": []
		})";
	}

	auto Loaded = LoadPatch(Path);
	REQUIRE(Loaded.has_value());
	REQUIRE(Loaded->Model.GetNodes().size() == 1);

	std::filesystem::remove(Path);
}

TEST_CASE("Patch serializer: skips unknown node types and continues", "[patch]")
{
	const auto Path = TempPatchPath();
	{
		std::ofstream F(Path);
		F << R"({
			"version": 1,
			"nodes": [
				{"id": 1, "type": "Oscillator", "x": 0, "y": 0, "params": {}},
				{"id": 2, "type": "TimeMachine9000", "x": 0, "y": 0, "params": {}},
				{"id": 3, "type": "Output", "x": 0, "y": 0, "params": {}}
			],
			"links": []
		})";
	}

	auto Loaded = LoadPatch(Path);
	REQUIRE(Loaded.has_value());
	REQUIRE(Loaded->Model.GetNodes().size() == 2);
	REQUIRE(Loaded->Model.GetNodes().count(1) == 1);
	REQUIRE(Loaded->Model.GetNodes().count(3) == 1);

	std::filesystem::remove(Path);
}

TEST_CASE("Patch serializer: metadata round-trips through save/load", "[patch][metadata]")
{
	FGraphModel Original;
	auto Out = std::make_shared<FOutput>();
	Original.AddNode(Out);
	auto& Meta = Original.GetMetadata();
	Meta.Name = "Soft Pad";
	Meta.Author = "Mark";
	Meta.Notes = "Reverb tail tuned for room ambience.";
	Meta.Bpm = 96.5f;
	Meta.SampleRateHint = 48000.0;

	const auto Path = TempPatchPath();
	REQUIRE(SavePatch(Original, Path));

	auto Loaded = LoadPatch(Path);
	REQUIRE(Loaded.has_value());
	const auto& LoadedMeta = Loaded->Model.GetMetadata();
	REQUIRE(LoadedMeta.Name == "Soft Pad");
	REQUIRE(LoadedMeta.Author == "Mark");
	REQUIRE(LoadedMeta.Notes == "Reverb tail tuned for room ambience.");
	REQUIRE_THAT(LoadedMeta.Bpm, Catch::Matchers::WithinAbs(96.5f, 1e-3f));
	REQUIRE_THAT(LoadedMeta.SampleRateHint, Catch::Matchers::WithinAbs(48000.0, 1e-3));

	std::filesystem::remove(Path);
}

TEST_CASE("Patch serializer: metadata fields default cleanly when absent", "[patch][metadata]")
{
	const auto Path = TempPatchPath();
	{
		std::ofstream F(Path);
		F << R"({
			"version": 1,
			"nodes": [{"id": 1, "type": "Output", "x": 0, "y": 0, "params": {}}],
			"links": []
		})";
	}

	auto Loaded = LoadPatch(Path);
	REQUIRE(Loaded.has_value());
	const auto& Meta = Loaded->Model.GetMetadata();
	REQUIRE(Meta.Name.empty());
	REQUIRE(Meta.Author.empty());
	REQUIRE(Meta.Notes.empty());
	REQUIRE(Meta.SampleRateHint == 0.0);

	std::filesystem::remove(Path);
}

TEST_CASE("AddNodeWithId rejects duplicate ids and a second Output", "[graph][addwithid]")
{
	FGraphModel Model;
	REQUIRE(Model.AddNodeWithId(7, std::make_shared<FGain>()) == 7);
	// Same id again — rejected.
	REQUIRE(Model.AddNodeWithId(7, std::make_shared<FGain>()) == 0);
	// First Output OK.
	REQUIRE(Model.AddNodeWithId(8, std::make_shared<FOutput>()) == 8);
	// Second Output rejected.
	REQUIRE(Model.AddNodeWithId(9, std::make_shared<FOutput>()) == 0);
	// AddNode (auto-id) should also reject another Output.
	REQUIRE(Model.AddNode(std::make_shared<FOutput>()) == 0);
	// NextNodeId should now be > 8 so subsequent AddNode doesn't collide.
	const FNodeId NewId = Model.AddNode(std::make_shared<FGain>());
	REQUIRE(NewId > 8);
}
