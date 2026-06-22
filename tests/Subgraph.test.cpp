#include "io/SubgraphSerializer.h"

#include "dsp/Gain.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/Subgraph.h"
#include "dsp/Svf.h"
#include "dsp/internal/SubgraphBoundary.h"
#include "graph/SubgraphDefinition.h"
#include "io/PatchSerializer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <memory>
#include <random>

using Catch::Matchers::WithinAbs;
using NodeSynth::DeserializeSubgraphDefinition;
using NodeSynth::EPortType;
using NodeSynth::FGain;
using NodeSynth::FGraphModel;
using NodeSynth::FNodeId;
using NodeSynth::FOscillator;
using NodeSynth::FOutput;
using NodeSynth::FProcessContext;
using NodeSynth::FSubgraph;
using NodeSynth::FSubgraphDefinition;
using NodeSynth::FSubgraphPin;
using NodeSynth::FSvf;
using NodeSynth::LoadPatch;
using NodeSynth::LoadSubgraph;
using NodeSynth::SavePatch;
using NodeSynth::SaveSubgraph;
using NodeSynth::SerializeSubgraphDefinition;
using NodeSynth::SyncSubgraphBoundaries;
namespace Internal = NodeSynth::Internal;

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

namespace
{
	std::filesystem::path TempPatchJsonPath()
	{
		std::random_device Rd;
		std::mt19937_64 Rng(Rd());
		std::filesystem::path Dir = std::filesystem::temp_directory_path() / "nodesynth_tests";
		std::filesystem::create_directories(Dir);
		return Dir / ("subpatch_" + std::to_string(Rng()) + ".json");
	}

	// Reads the buffer feeding the (single) Output node after processing. The
	// compiled snapshot shares the model's node instances, so the Output node's
	// input pointer reflects the last processed block.
	const float* OutputInputBuffer(FGraphModel& Model)
	{
		for (const auto& [Id, Rec] : Model.GetNodes())
		{
			if (Rec.Node && std::string(Rec.Node->GetTypeName()) == "Output")
			{
				return Rec.Node->GetInputBuffer(0);
			}
		}
		return nullptr;
	}
}

TEST_CASE("Subgraph definitions: rename re-keys the map and updates the definition", "[subgraph]")
{
	FGraphModel M;
	auto Def = std::make_shared<FSubgraphDefinition>();
	Def->Name = "Alpha";
	M.AddSubgraphDefinition(Def);

	// Collision and no-op renames are rejected.
	auto Other = std::make_shared<FSubgraphDefinition>();
	Other->Name = "Beta";
	M.AddSubgraphDefinition(Other);
	REQUIRE_FALSE(M.RenameSubgraphDefinition("Alpha", "Beta"));   // collision
	REQUIRE_FALSE(M.RenameSubgraphDefinition("Alpha", "Alpha"));  // no-op
	REQUIRE_FALSE(M.RenameSubgraphDefinition("Alpha", ""));       // empty
	REQUIRE_FALSE(M.RenameSubgraphDefinition("Ghost", "Gamma"));  // unknown

	REQUIRE(M.RenameSubgraphDefinition("Alpha", "Gamma"));
	REQUIRE(M.FindSubgraphDefinition("Alpha") == nullptr);
	REQUIRE(M.FindSubgraphDefinition("Gamma") == Def);  // same pointer, re-keyed
	REQUIRE(Def->Name == "Gamma");                       // name updated in place
}

TEST_CASE("Patch round-trip: embedded subgraph survives save/load and stays audio-identical",
	"[subgraph][patch]")
{
	constexpr float Freq = 220.0f;
	constexpr float GainValue = 0.5f;

	// Build a "GainBox" definition (In -> Gain -> Out) registered in the patch.
	auto Def = std::make_shared<FSubgraphDefinition>();
	Def->Name = "GainBox";
	Def->InputPins = { { "In", EPortType::Audio, "" } };
	Def->OutputPins = { { "Out", EPortType::Audio, "" } };
	{
		auto In = std::make_shared<Internal::FSubgraphInputs>();
		auto Out = std::make_shared<Internal::FSubgraphOutputs>();
		auto G = std::make_shared<FGain>();
		G->SetParamValue(FGain::Param_Gain, GainValue);
		const FNodeId InId = Def->InternalGraph.AddNode(In);
		const FNodeId OutId = Def->InternalGraph.AddNode(Out);
		const FNodeId GId = Def->InternalGraph.AddNode(G);
		SyncSubgraphBoundaries(*Def);
		REQUIRE(Def->InternalGraph.AddLink(InId, 0, GId, 0) != 0);
		REQUIRE(Def->InternalGraph.AddLink(GId, 0, OutId, 0) != 0);
	}

	FGraphModel Patch;
	Patch.AddSubgraphDefinition(Def);
	auto Osc = std::make_shared<FOscillator>();
	Osc->SetParamValue(FOscillator::Param_Frequency, Freq);
	auto SubNode = std::make_shared<FSubgraph>();
	SubNode->SetDefinition(Def);
	auto PatchOut = std::make_shared<FOutput>();
	const FNodeId OscId = Patch.AddNode(Osc);
	const FNodeId SubId = Patch.AddNode(SubNode);
	const FNodeId OutId = Patch.AddNode(PatchOut);
	REQUIRE(Patch.AddLink(OscId, 0, SubId, 0) != 0);
	REQUIRE(Patch.AddLink(SubId, 0, OutId, 0) != 0);

	const auto Path = TempPatchJsonPath();
	REQUIRE(SavePatch(Patch, Path));

	auto Loaded = LoadPatch(Path);
	REQUIRE(Loaded.has_value());

	// Definition restored into the loaded patch's map.
	REQUIRE(Loaded->Model.GetSubgraphDefinitions().count("GainBox") == 1);

	// The instance node was rebound to the loaded definition.
	bool FoundBoundInstance = false;
	for (const auto& [Id, Rec] : Loaded->Model.GetNodes())
	{
		if (auto* S = dynamic_cast<FSubgraph*>(Rec.Node.get()))
		{
			REQUIRE(S->GetDefinition() != nullptr);
			REQUIRE(S->GetDefinition()->Name == "GainBox");
			FoundBoundInstance = true;
		}
	}
	REQUIRE(FoundBoundInstance);

	// Reference: Osc -> Gain(0.5) -> Output, hand-wired.
	FGraphModel Ref;
	auto RefOsc = std::make_shared<FOscillator>();
	RefOsc->SetParamValue(FOscillator::Param_Frequency, Freq);
	auto RefGain = std::make_shared<FGain>();
	RefGain->SetParamValue(FGain::Param_Gain, GainValue);
	auto RefOut = std::make_shared<FOutput>();
	const FNodeId R0 = Ref.AddNode(RefOsc);
	const FNodeId R1 = Ref.AddNode(RefGain);
	const FNodeId R2 = Ref.AddNode(RefOut);
	REQUIRE(Ref.AddLink(R0, 0, R1, 0) != 0);
	REQUIRE(Ref.AddLink(R1, 0, R2, 0) != 0);

	auto LoadedSnap = Loaded->Model.Compile(48000.0);
	auto RefSnap = Ref.Compile(48000.0);
	REQUIRE_FALSE(LoadedSnap->OrderedNodes.empty());
	REQUIRE_FALSE(RefSnap->OrderedNodes.empty());

	FProcessContext Ctx;
	Ctx.SampleRate = 48000.0;
	for (int Block = 0; Block < 4; ++Block)
	{
		LoadedSnap->Process(Ctx);
		RefSnap->Process(Ctx);
		const float* LoadedBuf = OutputInputBuffer(Loaded->Model);
		const float* RefBuf = OutputInputBuffer(Ref);
		REQUIRE(LoadedBuf != nullptr);
		REQUIRE(RefBuf != nullptr);
		for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
		{
			REQUIRE_THAT(LoadedBuf[I], WithinAbs(RefBuf[I], 1e-6f));
		}
	}

	std::filesystem::remove(Path);
}
