#include "dsp/Adsr.h"
#include "dsp/Constant.h"
#include "dsp/Gain.h"
#include "dsp/Lfo.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/Scale.h"
#include "dsp/Svf.h"
#include "ui/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using NodeSynth::FAdsr;
using NodeSynth::FConstant;
using NodeSynth::FGain;
using NodeSynth::FLfo;
using NodeSynth::FOscillator;
using NodeSynth::FOutput;
using NodeSynth::FScale;
using NodeSynth::FSvf;
using NodeSynth::INode;

TEST_CASE("Clone: non-cloneable nodes return nullptr", "[clone]")
{
	REQUIRE(FOutput().Clone() == nullptr);
}

TEST_CASE("Clone: oscillator preserves Shape, Frequency, Amplitude", "[clone]")
{
	FOscillator Source;
	Source.SetParamValue(FOscillator::Param_Shape, 2.0f);     // Square
	Source.SetParamValue(FOscillator::Param_Frequency, 220.0f);
	Source.SetParamValue(FOscillator::Param_Amplitude, 0.42f);

	auto Cloned = Source.Clone();
	REQUIRE(Cloned != nullptr);
	REQUIRE_THAT(Cloned->GetParamValue(FOscillator::Param_Shape),
		Catch::Matchers::WithinAbs(2.0f, 1e-6f));
	REQUIRE_THAT(Cloned->GetParamValue(FOscillator::Param_Frequency),
		Catch::Matchers::WithinAbs(220.0f, 1e-3f));
	REQUIRE_THAT(Cloned->GetParamValue(FOscillator::Param_Amplitude),
		Catch::Matchers::WithinAbs(0.42f, 1e-6f));
}

TEST_CASE("Clone: ADSR preserves all four params", "[clone]")
{
	FAdsr Source;
	Source.SetParamValue(FAdsr::Param_AttackMs, 12.5f);
	Source.SetParamValue(FAdsr::Param_DecayMs, 350.0f);
	Source.SetParamValue(FAdsr::Param_Sustain, 0.6f);
	Source.SetParamValue(FAdsr::Param_ReleaseMs, 800.0f);

	auto Cloned = Source.Clone();
	REQUIRE(Cloned != nullptr);
	REQUIRE_THAT(Cloned->GetParamValue(FAdsr::Param_AttackMs),
		Catch::Matchers::WithinAbs(12.5f, 1e-3f));
	REQUIRE_THAT(Cloned->GetParamValue(FAdsr::Param_DecayMs),
		Catch::Matchers::WithinAbs(350.0f, 1e-3f));
	REQUIRE_THAT(Cloned->GetParamValue(FAdsr::Param_Sustain),
		Catch::Matchers::WithinAbs(0.6f, 1e-6f));
	REQUIRE_THAT(Cloned->GetParamValue(FAdsr::Param_ReleaseMs),
		Catch::Matchers::WithinAbs(800.0f, 1e-3f));
}

TEST_CASE("Clone: SVF preserves Cutoff and Resonance", "[clone]")
{
	FSvf Source;
	Source.SetParamValue(FSvf::Param_Cutoff, 5000.0f);
	Source.SetParamValue(FSvf::Param_Resonance, 0.85f);

	auto Cloned = Source.Clone();
	REQUIRE(Cloned != nullptr);
	REQUIRE_THAT(Cloned->GetParamValue(FSvf::Param_Cutoff),
		Catch::Matchers::WithinAbs(5000.0f, 1e-3f));
	REQUIRE_THAT(Cloned->GetParamValue(FSvf::Param_Resonance),
		Catch::Matchers::WithinAbs(0.85f, 1e-6f));
}

TEST_CASE("Clone: Gain / Constant / Scale / LFO round-trip params", "[clone]")
{
	{
		FGain Source;
		Source.SetParamValue(FGain::Param_Gain, 1.7f);
		auto Cloned = Source.Clone();
		REQUIRE_THAT(Cloned->GetParamValue(FGain::Param_Gain),
			Catch::Matchers::WithinAbs(1.7f, 1e-6f));
	}
	{
		FConstant Source;
		Source.SetParamValue(FConstant::Param_Value, -3.14f);
		auto Cloned = Source.Clone();
		REQUIRE_THAT(Cloned->GetParamValue(FConstant::Param_Value),
			Catch::Matchers::WithinAbs(-3.14f, 1e-5f));
	}
	{
		FScale Source;
		Source.SetParamValue(FScale::Param_InMin, 0.1f);
		Source.SetParamValue(FScale::Param_InMax, 0.9f);
		Source.SetParamValue(FScale::Param_OutMin, -2.0f);
		Source.SetParamValue(FScale::Param_OutMax, 5.0f);
		auto Cloned = Source.Clone();
		REQUIRE_THAT(Cloned->GetParamValue(FScale::Param_InMin),
			Catch::Matchers::WithinAbs(0.1f, 1e-6f));
		REQUIRE_THAT(Cloned->GetParamValue(FScale::Param_OutMax),
			Catch::Matchers::WithinAbs(5.0f, 1e-6f));
	}
	{
		FLfo Source;
		Source.SetParamValue(FLfo::Param_Shape, 2.0f); // Saw
		Source.SetParamValue(FLfo::Param_RateHz, 4.0f);
		Source.SetParamValue(FLfo::Param_Amount, 0.5f);
		auto Cloned = Source.Clone();
		REQUIRE_THAT(Cloned->GetParamValue(FLfo::Param_Shape),
			Catch::Matchers::WithinAbs(2.0f, 1e-6f));
		REQUIRE_THAT(Cloned->GetParamValue(FLfo::Param_RateHz),
			Catch::Matchers::WithinAbs(4.0f, 1e-6f));
		REQUIRE_THAT(Cloned->GetParamValue(FLfo::Param_Amount),
			Catch::Matchers::WithinAbs(0.5f, 1e-6f));
	}
}

TEST_CASE("Clone: returned node is a fresh instance, not the same shared_ptr", "[clone]")
{
	FOscillator Source;
	auto Cloned = Source.Clone();
	REQUIRE(Cloned != nullptr);
	REQUIRE(static_cast<const void*>(Cloned.get()) != static_cast<const void*>(&Source));

	// Mutating the clone doesn't affect the source.
	Cloned->SetParamValue(FOscillator::Param_Frequency, 1234.0f);
	REQUIRE(Source.GetParamValue(FOscillator::Param_Frequency) != 1234.0f);
}

TEST_CASE("Clone: cloned ADSR's transient state is reset by Prepare", "[clone]")
{
	// Source ADSR ticks through to Sustain stage.
	FAdsr Source;
	Source.Prepare(48000.0);
	Source.SetParamValue(FAdsr::Param_AttackMs, 0.5f);
	Source.SetParamValue(FAdsr::Param_DecayMs, 0.5f);
	Source.SetParamValue(FAdsr::Param_Sustain, 0.7f);
	Source.SetParamValue(FAdsr::Param_ReleaseMs, 0.5f);

	std::vector<float> Gate(NodeSynth::BlockSize, 1.0f);
	Source.SetInputBuffer(0, Gate.data());
	NodeSynth::FProcessContext Ctx;
	Ctx.SampleRate = 48000.0;
	for (int B = 0; B < 50; ++B) // ~2700 samples — well past Attack + Decay
	{
		Source.Process(Ctx);
	}
	REQUIRE(Source.GetStage() == FAdsr::EStage::Sustain);

	// Clone and Prepare it. The cloned stage should be Idle (reset state).
	auto Cloned = Source.Clone();
	REQUIRE(Cloned != nullptr);
	auto* ClonedAdsr = static_cast<FAdsr*>(Cloned.get());
	ClonedAdsr->Prepare(48000.0);
	REQUIRE(ClonedAdsr->GetStage() == FAdsr::EStage::Idle);
}

TEST_CASE("Clone via MakeNodeByTypeName + param-by-name matches direct Clone", "[clone]")
{
	// Sanity: the default Clone implementation is essentially equivalent to
	// "make fresh by type name, copy each param by name." Verify both paths
	// produce indistinguishable param values.
	FOscillator Source;
	Source.SetParamValue(FOscillator::Param_Frequency, 880.0f);
	Source.SetParamValue(FOscillator::Param_Amplitude, 0.25f);

	auto ClonedA = Source.Clone();
	auto ClonedB = NodeSynth::MakeNodeByTypeName("Oscillator");
	REQUIRE(ClonedB != nullptr);
	ClonedB->SetParamValue(FOscillator::Param_Frequency, 880.0f);
	ClonedB->SetParamValue(FOscillator::Param_Amplitude, 0.25f);

	REQUIRE(ClonedA->GetParamValue(FOscillator::Param_Frequency)
		== ClonedB->GetParamValue(FOscillator::Param_Frequency));
	REQUIRE(ClonedA->GetParamValue(FOscillator::Param_Amplitude)
		== ClonedB->GetParamValue(FOscillator::Param_Amplitude));
}
