// Hidden-by-default Catch2 test that writes the bundled preset .json files
// out to disk. Tagged [.][preset-emit] so the standard `nodesynth_tests` run
// skips it; regenerate the bundled presets by selecting the tag explicitly:
//
//   ./build/Release/nodesynth_tests.exe "[preset-emit]"
//
// from the repo root. Output goes to ./presets/<category>/<name>.json. The
// resulting JSONs are committed to git — runtime preset loading reads from
// the binary's bundled `presets/` (copied in at build time), not from this
// test, so the only purpose here is reproducibility of preset content.

#include "io/PatchSerializer.h"

#include "dsp/Adsr.h"
#include "dsp/Gain.h"
#include "dsp/Meter.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/VoiceAllocator.h"
#include "graph/Graph.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace NodeSynth;

namespace
{
	struct FBuiltPatch
	{
		FGraphModel Model;
		// Direct pointers into the model's nodes for easy param tweaking
		// after the topology is built.
		FAdsr* Adsr = nullptr;
		FOscillator* Osc = nullptr;
		FGain* Gain = nullptr;
		FVoiceAllocator* Alloc = nullptr;
	};

	// Builds the same node graph as main.cpp::SeedDefaultPatch — keep this in
	// sync if SeedDefaultPatch's topology ever changes. Returns the model
	// plus pointers to the tweakable nodes so each preset variant can adjust
	// shape / envelope / glide without rebuilding the graph.
	FBuiltPatch BuildSeededPatch()
	{
		FBuiltPatch P;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto GainNode = std::make_shared<FGain>();
		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();

		GainNode->SetParamValue(FGain::Param_Gain, 0.15f);

		const FNodeId AllocId = P.Model.AddNode(Alloc, 60.0f, 240.0f);
		const FNodeId AdsrId = P.Model.AddNode(Adsr, 340.0f, 60.0f);
		const FNodeId OscId = P.Model.AddNode(Osc, 340.0f, 240.0f);
		const FNodeId GainId = P.Model.AddNode(GainNode, 620.0f, 180.0f);
		const FNodeId MeterId = P.Model.AddNode(MeterNode, 860.0f, 180.0f);
		const FNodeId OutId = P.Model.AddNode(Out, 1100.0f, 180.0f);

		P.Model.SetNodePerVoice(AdsrId, true);
		P.Model.SetNodePerVoice(OscId, true);

		P.Model.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		P.Model.AddLink(AllocId, FVoiceAllocator::Output_Frequency, OscId, FOscillator::Input_Frequency);
		P.Model.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		P.Model.AddLink(OscId, 0, GainId, 0);
		P.Model.AddLink(GainId, 0, MeterId, 0);
		P.Model.AddLink(MeterId, 0, OutId, 0);

		P.Adsr = Adsr.get();
		P.Osc = Osc.get();
		P.Gain = GainNode.get();
		P.Alloc = Alloc.get();
		return P;
	}

	// Param indices match the Param_<Name> enums in each header. Repeating
	// them here as named locals keeps the call sites below readable.
	constexpr uint32_t Adsr_Attack = FAdsr::Param_AttackMs;
	constexpr uint32_t Adsr_Decay = FAdsr::Param_DecayMs;
	constexpr uint32_t Adsr_Sustain = FAdsr::Param_Sustain;
	constexpr uint32_t Adsr_Release = FAdsr::Param_ReleaseMs;
	constexpr uint32_t Osc_Shape = FOscillator::Param_Shape;
	constexpr uint32_t Gain_Level = FGain::Param_Gain;
	constexpr uint32_t Alloc_Glide = FVoiceAllocator::Param_Glide;

	// Oscillator shape indices: Sine=0, Saw=1, Square=2, Triangle=3, Noise=4.
	enum EShape : int { Sine = 0, Saw = 1, Square = 2, Triangle = 3, Noise = 4 };

	void EmitPreset(
		const std::filesystem::path& Root,
		const std::string& Category,
		const std::string& Name,
		const std::string& Notes,
		float Shape,
		float AttackMs,
		float DecayMs,
		float Sustain,
		float ReleaseMs,
		float MasterGain,
		float GlideMs)
	{
		FBuiltPatch P = BuildSeededPatch();
		P.Osc->SetParamValue(Osc_Shape, Shape);
		P.Adsr->SetParamValue(Adsr_Attack, AttackMs);
		P.Adsr->SetParamValue(Adsr_Decay, DecayMs);
		P.Adsr->SetParamValue(Adsr_Sustain, Sustain);
		P.Adsr->SetParamValue(Adsr_Release, ReleaseMs);
		P.Gain->SetParamValue(Gain_Level, MasterGain);
		P.Alloc->SetParamValue(Alloc_Glide, GlideMs);

		FPatchMetadata& Meta = P.Model.GetMetadata();
		Meta.Name = Name;
		Meta.Author = "NodeSynth";
		Meta.Notes = Notes;
		Meta.Bpm = 120.0f;
		Meta.SampleRateHint = 48000.0;

		const std::filesystem::path Dir = Root / Category;
		std::filesystem::create_directories(Dir);
		const std::filesystem::path Out = Dir / (Name + ".json");
		REQUIRE(SavePatch(P.Model, Out));
	}
}

TEST_CASE("Emit bundled presets to ./presets/", "[.][preset-emit]")
{
	const std::filesystem::path Root = std::filesystem::current_path() / "presets";
	std::filesystem::create_directories(Root);

	// Init: identical to main.cpp::SeedDefaultPatch defaults.
	EmitPreset(Root, "Init", "Init Patch",
		"Polyphonic sine, 8 voices, master gain 0.15. The seeded default patch.",
		Sine, 5.0f, 200.0f, 0.7f, 400.0f, 0.15f, 0.0f);

	// Bass: short release, square sub.
	EmitPreset(Root, "Bass", "Square Sub",
		"Square wave with snappy envelope. Pull the master gain up if you only play one voice at a time.",
		Square, 1.0f, 80.0f, 0.6f, 120.0f, 0.18f, 0.0f);

	EmitPreset(Root, "Bass", "Acid Saw",
		"Saw with a fast attack and quick release. Pair with an SVF for resonant acid lines.",
		Saw, 1.0f, 100.0f, 0.5f, 80.0f, 0.15f, 0.0f);

	// Lead: faster attack, brighter shapes.
	EmitPreset(Root, "Lead", "Saw Lead",
		"Saw lead with a touch of glide. Plays well above middle C.",
		Saw, 8.0f, 200.0f, 0.7f, 350.0f, 0.18f, 30.0f);

	EmitPreset(Root, "Lead", "Square Lead",
		"Hollow square lead. 60 ms glide gives it a vocal quality on legato runs.",
		Square, 12.0f, 220.0f, 0.65f, 300.0f, 0.18f, 60.0f);

	// Pad: long attack/release, sustained.
	EmitPreset(Root, "Pad", "Soft Pad",
		"Triangle pad with slow attack and long release. Holds a chord nicely.",
		Triangle, 600.0f, 1000.0f, 0.85f, 2200.0f, 0.18f, 0.0f);

	EmitPreset(Root, "Pad", "String",
		"Saw-based string pad. Slow attack, long release, fairly bright.",
		Saw, 800.0f, 1200.0f, 0.8f, 2500.0f, 0.16f, 0.0f);

	// FX: percussive plucks.
	EmitPreset(Root, "FX", "Pluck",
		"Triangle pluck — fast attack, decay-only envelope (sustain=0).",
		Triangle, 1.0f, 250.0f, 0.0f, 250.0f, 0.18f, 0.0f);
}
