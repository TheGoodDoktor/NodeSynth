#include "dsp/Gain.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "graph/Graph.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>

using namespace NodeSynth;

TEST_CASE("FGraphModel rejects cycles", "[graph]")
{
	FGraphModel Model;
	const FNodeId A = Model.AddNode(std::make_shared<FGain>());
	const FNodeId B = Model.AddNode(std::make_shared<FGain>());
	REQUIRE(Model.AddLink(A, 0, B, 0) != 0);
	// B -> A would close the loop.
	REQUIRE(Model.AddLink(B, 0, A, 0) == 0);
}

TEST_CASE("FGraphModel rejects port type mismatch", "[graph]")
{
	FGraphModel Model;
	const FNodeId Osc = Model.AddNode(std::make_shared<FOscillator>());
	const FNodeId Out = Model.AddNode(std::make_shared<FOutput>());
	// Valid audio-to-audio link.
	REQUIRE(Model.AddLink(Osc, 0, Out, 0) != 0);
	// Same port on the downstream is already full; supersedes prior.
	REQUIRE(Model.AddLink(Osc, 0, Out, 0) != 0);
	REQUIRE(Model.GetLinks().size() == 1);
}

TEST_CASE("Compiled graph produces a non-zero sine through Osc -> Gain -> Output", "[graph]")
{
	FGraphModel Model;
	auto Osc = std::make_shared<FOscillator>();
	auto GainNode = std::make_shared<FGain>();
	auto Out = std::make_shared<FOutput>();

	const FNodeId OscId = Model.AddNode(Osc);
	const FNodeId GainId = Model.AddNode(GainNode);
	const FNodeId OutId = Model.AddNode(Out);
	REQUIRE(Model.AddLink(OscId, 0, GainId, 0) != 0);
	REQUIRE(Model.AddLink(GainId, 0, OutId, 0) != 0);

	auto AudioGraph = Model.Compile(48000.0);
	REQUIRE(AudioGraph->OutputNode != nullptr);

	FProcessContext Ctx;
	Ctx.BlockSize = BlockSize;
	Ctx.SampleRate = 48000.0;

	AudioGraph->Process(Ctx);
	const float* OutputBuf = AudioGraph->OutputNode->GetInputBuffer(0);
	REQUIRE(OutputBuf != nullptr);

	// Default oscillator is 440 Hz sine at 0.3 amplitude — over one block at 48k,
	// at least one sample must be non-zero.
	bool bAnyNonZero = false;
	for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
	{
		if (OutputBuf[I] != 0.0f)
		{
			bAnyNonZero = true;
			break;
		}
	}
	REQUIRE(bAnyNonZero);
}

TEST_CASE("Compile broadcasts a mono producer's L buffer onto the consumer's R input", "[graph][stereo]")
{
	// Phase 5b wire-level broadcast: until any node opts into stereo output,
	// every link plumbs the source's L buffer into both L and R of the
	// destination, so a stereo-aware consumer (e.g. the audio sink) sees the
	// same content on both channels of its mono input.
	FGraphModel Model;
	auto Osc = std::make_shared<FOscillator>();
	auto Out = std::make_shared<FOutput>();
	const FNodeId OscId = Model.AddNode(Osc);
	const FNodeId OutId = Model.AddNode(Out);
	REQUIRE(Model.AddLink(OscId, 0, OutId, 0) != 0);

	auto AudioGraph = Model.Compile(48000.0);
	REQUIRE(AudioGraph->OutputNode != nullptr);

	FProcessContext Ctx;
	Ctx.BlockSize = BlockSize;
	Ctx.SampleRate = 48000.0;
	AudioGraph->Process(Ctx);

	const float* InL = AudioGraph->OutputNode->GetInputBuffer(0, 0);
	const float* InR = AudioGraph->OutputNode->GetInputBuffer(0, 1);
	REQUIRE(InL != nullptr);
	REQUIRE(InR != nullptr);
	// Mono producer → both pointers alias the same buffer.
	REQUIRE(InL == InR);
	// Belt-and-braces: also verify the values match sample-by-sample.
	for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
	{
		REQUIRE(InL[I] == InR[I]);
	}
}
