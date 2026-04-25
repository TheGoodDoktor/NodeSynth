#include "dsp/Gain.h"
#include "dsp/MidiInput.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/VirtualKeyboard.h"
#include "graph/Graph.h"
#include "io/PatchSerializer.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>

using NodeSynth::FGain;
using NodeSynth::FGraphModel;
using NodeSynth::FMidiInput;
using NodeSynth::FNodeId;
using NodeSynth::FOscillator;
using NodeSynth::FOutput;
using NodeSynth::FVirtualKeyboard;
using NodeSynth::LoadPatch;
using NodeSynth::SavePatch;

namespace
{
	std::filesystem::path TempPatchPath()
	{
		std::random_device Rd;
		std::mt19937_64 Rng(Rd());
		std::filesystem::path Dir = std::filesystem::temp_directory_path() / "nodesynth_tests";
		std::filesystem::create_directories(Dir);
		return Dir / ("pervoice_" + std::to_string(Rng()) + ".json");
	}
}

TEST_CASE("Per-voice flag: default is false", "[pervoice]")
{
	FGraphModel Model;
	const FNodeId Id = Model.AddNode(std::make_shared<FOscillator>());
	REQUIRE_FALSE(Model.GetNodes().at(Id).bPerVoice);
}

TEST_CASE("Per-voice flag: SetNodePerVoice toggles a cloneable node", "[pervoice]")
{
	FGraphModel Model;
	const FNodeId Id = Model.AddNode(std::make_shared<FOscillator>());

	REQUIRE(Model.SetNodePerVoice(Id, true));
	REQUIRE(Model.GetNodes().at(Id).bPerVoice);

	REQUIRE(Model.SetNodePerVoice(Id, false));
	REQUIRE_FALSE(Model.GetNodes().at(Id).bPerVoice);
}

TEST_CASE("Per-voice flag: rejected on non-cloneable nodes", "[pervoice]")
{
	FGraphModel Model;
	const FNodeId OutputId = Model.AddNode(std::make_shared<FOutput>());
	const FNodeId KbdId = Model.AddNode(std::make_shared<FVirtualKeyboard>());
	const FNodeId MidiId = Model.AddNode(std::make_shared<FMidiInput>());

	REQUIRE_FALSE(Model.SetNodePerVoice(OutputId, true));
	REQUIRE_FALSE(Model.SetNodePerVoice(KbdId, true));
	REQUIRE_FALSE(Model.SetNodePerVoice(MidiId, true));

	REQUIRE_FALSE(Model.GetNodes().at(OutputId).bPerVoice);
	REQUIRE_FALSE(Model.GetNodes().at(KbdId).bPerVoice);
	REQUIRE_FALSE(Model.GetNodes().at(MidiId).bPerVoice);
}

TEST_CASE("Per-voice flag: returns false for unknown node id", "[pervoice]")
{
	FGraphModel Model;
	REQUIRE_FALSE(Model.SetNodePerVoice(9999, true));
}

TEST_CASE("Per-voice flag: clearing is always allowed (even on non-cloneable)", "[pervoice]")
{
	// Defensive: if a future code path somehow set bPerVoice on a non-cloneable
	// node, we want the user to be able to clear it back without rejection.
	FGraphModel Model;
	const FNodeId OutputId = Model.AddNode(std::make_shared<FOutput>());
	REQUIRE(Model.SetNodePerVoice(OutputId, false));
}

TEST_CASE("Per-voice flag: round-trips through save/load", "[pervoice][patch]")
{
	FGraphModel Original;
	auto Osc = std::make_shared<FOscillator>();
	auto GainN = std::make_shared<FGain>();
	auto Out = std::make_shared<FOutput>();
	const FNodeId OscId = Original.AddNode(Osc);
	const FNodeId GainId = Original.AddNode(GainN);
	const FNodeId OutId = Original.AddNode(Out);
	Original.AddLink(OscId, 0, GainId, 0);
	Original.AddLink(GainId, 0, OutId, 0);

	REQUIRE(Original.SetNodePerVoice(OscId, true));
	REQUIRE_FALSE(Original.GetNodes().at(GainId).bPerVoice);

	const auto Path = TempPatchPath();
	REQUIRE(SavePatch(Original, Path));

	auto Loaded = LoadPatch(Path);
	REQUIRE(Loaded.has_value());
	REQUIRE(Loaded->Model.GetNodes().at(OscId).bPerVoice);
	REQUIRE_FALSE(Loaded->Model.GetNodes().at(GainId).bPerVoice);
	REQUIRE_FALSE(Loaded->Model.GetNodes().at(OutId).bPerVoice);

	std::filesystem::remove(Path);
}

TEST_CASE("Per-voice flag: missing key in old patch files defaults to false", "[pervoice][patch]")
{
	// Simulate a v1 file written before per_voice existed — no key on any node.
	// Loader should accept it and leave bPerVoice false.
	const auto Path = TempPatchPath();
	{
		std::ofstream F(Path);
		F << R"({
			"version": 1,
			"nodes": [
				{"id": 1, "type": "Oscillator", "x": 0, "y": 0, "params": {}},
				{"id": 2, "type": "Output",     "x": 0, "y": 0, "params": {}}
			],
			"links": []
		})";
	}

	auto Loaded = LoadPatch(Path);
	REQUIRE(Loaded.has_value());
	REQUIRE_FALSE(Loaded->Model.GetNodes().at(1).bPerVoice);
	REQUIRE_FALSE(Loaded->Model.GetNodes().at(2).bPerVoice);

	std::filesystem::remove(Path);
}
