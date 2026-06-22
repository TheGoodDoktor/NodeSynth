// Hidden-by-default Catch2 test that writes the bundled subgraph asset
// (subgraphs/StereoFilter.nspg) and a showcase preset that uses it
// (presets/Lead/Subgraph Demo.json). Tagged [.][subgraph-emit] so the standard
// run skips it; regenerate with:
//
//   ./build/Release/nodesynth_tests.exe "[subgraph-emit]"
//
// from the repo root. The outputs are committed to git and copied next to the
// binary at build time.

#include "io/PatchSerializer.h"
#include "io/SubgraphSerializer.h"

#include "dsp/Adsr.h"
#include "dsp/Gain.h"
#include "dsp/Lfo.h"
#include "dsp/Meter.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/Scale.h"
#include "dsp/Subgraph.h"
#include "dsp/Svf.h"
#include "dsp/VoiceAllocator.h"
#include "dsp/internal/SubgraphBoundary.h"
#include "graph/Graph.h"
#include "graph/SubgraphDefinition.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

using namespace NodeSynth;

namespace
{
	constexpr float ShapeSaw = 1.0f;       // Oscillator shape index
	constexpr float LfoSine = 0.0f;
	constexpr float LfoTriangle = 1.0f;

	// The bundled "Stereo Filter" building block: Audio in -> SVF -> Audio out,
	// with Cutoff and Resonance exposed as Control pins. The first reusable
	// subgraph (docs/PLAN-SUBGRAPHS.md SG.6).
	std::shared_ptr<FSubgraphDefinition> MakeStereoFilterDef()
	{
		auto Def = std::make_shared<FSubgraphDefinition>();
		Def->Name = "StereoFilter";
		Def->InputPins = {
			{ "Audio", EPortType::Audio, "Signal to filter." },
			{ "Cutoff", EPortType::Control, "Cutoff frequency (Hz)." },
			{ "Resonance", EPortType::Control, "Resonance / Q (0..1)." },
		};
		Def->OutputPins = {
			{ "Audio", EPortType::Audio, "Low-pass filtered output." },
		};

		auto In = std::make_shared<Internal::FSubgraphInputs>();
		auto Out = std::make_shared<Internal::FSubgraphOutputs>();
		auto Filt = std::make_shared<FSvf>();
		Filt->SetParamValue(FSvf::Param_Cutoff, 1200.0f);
		Filt->SetParamValue(FSvf::Param_Resonance, 0.6f);

		const FNodeId InId = Def->InternalGraph.AddNode(In, -260.0f, 40.0f);
		const FNodeId FiltId = Def->InternalGraph.AddNode(Filt, 20.0f, 40.0f);
		const FNodeId OutId = Def->InternalGraph.AddNode(Out, 300.0f, 40.0f);

		SyncSubgraphBoundaries(*Def);
		REQUIRE(Def->InternalGraph.AddLink(InId, 0, FiltId, FSvf::Input_Audio) != 0);
		REQUIRE(Def->InternalGraph.AddLink(InId, 1, FiltId, FSvf::Input_Cutoff) != 0);
		REQUIRE(Def->InternalGraph.AddLink(InId, 2, FiltId, FSvf::Input_Resonance) != 0);
		REQUIRE(Def->InternalGraph.AddLink(FiltId, FSvf::Output_LowPass, OutId, 0) != 0);
		return Def;
	}
}

TEST_CASE("Emit bundled StereoFilter subgraph + showcase preset", "[.][subgraph-emit]")
{
	const std::filesystem::path Cwd = std::filesystem::current_path();

	// --- 1) The bundled .nspg asset -----------------------------------------
	{
		const std::filesystem::path Dir = Cwd / "subgraphs";
		std::filesystem::create_directories(Dir);
		auto Def = MakeStereoFilterDef();
		REQUIRE(SaveSubgraph(*Def, Dir / "StereoFilter.nspg"));
	}

	// --- 2) The showcase preset using two instances in series --------------
	{
		FGraphModel M;
		auto Def = MakeStereoFilterDef();
		M.AddSubgraphDefinition(Def);

		// Polyphonic saw core -> mono master trim (the per-voice -> mono Audio
		// link synthesises a voice mixer at Compile time).
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto GainNode = std::make_shared<FGain>();
		Adsr->SetParamValue(FAdsr::Param_AttackMs, 600.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 1000.0f);
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.85f);
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 1800.0f);
		Osc->SetParamValue(FOscillator::Param_Shape, ShapeSaw);
		GainNode->SetParamValue(FGain::Param_Gain, 0.18f);

		const FNodeId AllocId = M.AddNode(Alloc, 60.0f, 280.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 320.0f, 80.0f);
		const FNodeId OscId = M.AddNode(Osc, 320.0f, 280.0f);
		const FNodeId GainId = M.AddNode(GainNode, 580.0f, 280.0f);
		M.SetNodePerVoice(AdsrId, true);
		M.SetNodePerVoice(OscId, true);
		M.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, OscId, FOscillator::Input_Frequency);
		M.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		M.AddLink(OscId, 0, GainId, 0);

		// Two LFOs at different rates drive the two filters' cutoffs.
		auto Lfo1 = std::make_shared<FLfo>();
		auto Scale1 = std::make_shared<FScale>();
		auto Lfo2 = std::make_shared<FLfo>();
		auto Scale2 = std::make_shared<FScale>();
		Lfo1->SetParamValue(FLfo::Param_Shape, LfoSine);
		Lfo1->SetParamValue(FLfo::Param_RateHz, 0.30f);
		Lfo1->SetParamValue(FLfo::Param_Amount, 1.0f);
		Scale1->SetParamValue(FScale::Param_InMin, -1.0f);
		Scale1->SetParamValue(FScale::Param_InMax, 1.0f);
		Scale1->SetParamValue(FScale::Param_OutMin, 300.0f);
		Scale1->SetParamValue(FScale::Param_OutMax, 2500.0f);
		Lfo2->SetParamValue(FLfo::Param_Shape, LfoTriangle);
		Lfo2->SetParamValue(FLfo::Param_RateHz, 0.55f);
		Lfo2->SetParamValue(FLfo::Param_Amount, 1.0f);
		Scale2->SetParamValue(FScale::Param_InMin, -1.0f);
		Scale2->SetParamValue(FScale::Param_InMax, 1.0f);
		Scale2->SetParamValue(FScale::Param_OutMin, 600.0f);
		Scale2->SetParamValue(FScale::Param_OutMax, 5000.0f);

		const FNodeId Lfo1Id = M.AddNode(Lfo1, 320.0f, 460.0f);
		const FNodeId Scale1Id = M.AddNode(Scale1, 580.0f, 460.0f);
		const FNodeId Lfo2Id = M.AddNode(Lfo2, 320.0f, 600.0f);
		const FNodeId Scale2Id = M.AddNode(Scale2, 1080.0f, 460.0f);

		// Two StereoFilter instances in series, sharing one definition.
		auto Sub1 = std::make_shared<FSubgraph>();
		auto Sub2 = std::make_shared<FSubgraph>();
		Sub1->SetDefinition(Def);
		Sub2->SetDefinition(Def);
		const FNodeId Sub1Id = M.AddNode(Sub1, 840.0f, 280.0f);
		const FNodeId Sub2Id = M.AddNode(Sub2, 1340.0f, 280.0f);

		// StereoFilter pins: Audio=0, Cutoff=1, Resonance=2 (in), Audio=0 (out).
		M.AddLink(GainId, 0, Sub1Id, 0);
		M.AddLink(Lfo1Id, 0, Scale1Id, 0);
		M.AddLink(Scale1Id, 0, Sub1Id, 1);
		M.AddLink(Sub1Id, 0, Sub2Id, 0);
		M.AddLink(Lfo2Id, 0, Scale2Id, 0);
		M.AddLink(Scale2Id, 0, Sub2Id, 1);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1600.0f, 280.0f);
		const FNodeId OutId = M.AddNode(Out, 1840.0f, 280.0f);
		M.AddLink(Sub2Id, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		FPatchMetadata& Meta = M.GetMetadata();
		Meta.Name = "Subgraph Demo";
		Meta.Author = "NodeSynth";
		Meta.Notes = "Saw poly core -> two StereoFilter subgraphs in series, each "
			"swept by its own LFO (0.30 Hz / 0.55 Hz). Double-click a filter node "
			"to dive in and edit the shared definition — both instances update.";
		Meta.Bpm = 120.0f;
		Meta.SampleRateHint = 48000.0;

		// Validate the patch compiles (subgraph expansion + per-voice partition).
		const std::shared_ptr<FAudioGraph> Snap = M.Compile(48000.0);
		INFO("Subgraph Demo compile error: " << M.GetLastCompileError().Message);
		REQUIRE(Snap);
		REQUIRE_FALSE(Snap->OrderedNodes.empty());

		const std::filesystem::path Dir = Cwd / "presets" / "Lead";
		std::filesystem::create_directories(Dir);
		REQUIRE(SavePatch(M, Dir / "Subgraph Demo.json"));
	}
}
