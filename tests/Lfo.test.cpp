#include "dsp/Lfo.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::ELfoShape;
using NodeSynth::FLfo;
using NodeSynth::FProcessContext;

namespace
{
	FProcessContext Ctx(uint32_t Block = BlockSize, double Sr = 48000.0)
	{
		FProcessContext C;
		C.BlockSize = Block;
		C.SampleRate = Sr;
		return C;
	}

	// Run a chain of full-blocksize blocks and collect the output. Useful for
	// checking the LFO across hundreds of milliseconds without one giant block.
	std::vector<float> RunBlocks(FLfo& Lfo, uint32_t NumBlocks)
	{
		std::vector<float> Out;
		Out.reserve(NumBlocks * BlockSize);
		for (uint32_t B = 0; B < NumBlocks; ++B)
		{
			Lfo.Process(Ctx());
			const float* Block = Lfo.GetOutputBuffer(0);
			for (uint32_t I = 0; I < BlockSize; ++I)
			{
				Out.push_back(Block[I]);
			}
		}
		return Out;
	}
}

TEST_CASE("FLfo: sine starts at 0 and rises", "[lfo][sine]")
{
	FLfo Lfo;
	Lfo.Prepare(48000.0);
	Lfo.SetParamValue(FLfo::Param_Shape, static_cast<float>(ELfoShape::Sine));
	Lfo.SetParamValue(FLfo::Param_RateHz, 1.0f);
	Lfo.SetParamValue(FLfo::Param_Amount, 1.0f);

	Lfo.Process(Ctx());
	const float* Out = Lfo.GetOutputBuffer(0);
	// Sample 0 is post-advance, so phase ≈ 1/48000 cycle. sin(2π * 1/48000) ≈ 1.3e-4.
	REQUIRE_THAT(Out[0], Catch::Matchers::WithinAbs(0.0f, 5e-4f));
	REQUIRE(Out[1] > Out[0]); // rising
}

TEST_CASE("FLfo: saw is linear from -1 toward +1 over one period", "[lfo][saw]")
{
	FLfo Lfo;
	Lfo.Prepare(48000.0);
	Lfo.SetParamValue(FLfo::Param_Shape, static_cast<float>(ELfoShape::Saw));
	Lfo.SetParamValue(FLfo::Param_RateHz, 1.0f);
	Lfo.SetParamValue(FLfo::Param_Amount, 1.0f);

	const auto Buf = RunBlocks(Lfo, 24000 / BlockSize + 1); // ~half a 1Hz period

	// Sample 0: phase ≈ 1/48000, output ≈ 2 * 1/48000 - 1 ≈ -0.99996.
	REQUIRE_THAT(Buf[0], Catch::Matchers::WithinAbs(-1.0f, 1e-3f));
	// Halfway through a 1Hz period at 48kHz is sample 24000 — output ≈ 0.
	REQUIRE_THAT(Buf[24000], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("FLfo: square is +1 in first half-period, -1 in second", "[lfo][square]")
{
	FLfo Lfo;
	Lfo.Prepare(48000.0);
	Lfo.SetParamValue(FLfo::Param_Shape, static_cast<float>(ELfoShape::Square));
	Lfo.SetParamValue(FLfo::Param_RateHz, 1.0f);
	Lfo.SetParamValue(FLfo::Param_Amount, 1.0f);

	const auto Buf = RunBlocks(Lfo, 48000 / BlockSize + 1);
	REQUIRE(Buf[0] == 1.0f);
	REQUIRE(Buf[10000] == 1.0f);
	REQUIRE(Buf[24001] == -1.0f);
	REQUIRE(Buf[40000] == -1.0f);
}

TEST_CASE("FLfo: triangle peaks at phase 0 and troughs at phase 0.5", "[lfo][triangle]")
{
	FLfo Lfo;
	Lfo.Prepare(48000.0);
	Lfo.SetParamValue(FLfo::Param_Shape, static_cast<float>(ELfoShape::Triangle));
	Lfo.SetParamValue(FLfo::Param_RateHz, 1.0f);
	Lfo.SetParamValue(FLfo::Param_Amount, 1.0f);

	const auto Buf = RunBlocks(Lfo, 48000 / BlockSize + 1);
	// Phase ≈ 0 + epsilon at sample 0 → output ≈ +1.
	REQUIRE_THAT(Buf[0], Catch::Matchers::WithinAbs(1.0f, 1e-3f));
	// Phase 0.5 at sample 24000 → output ≈ -1.
	REQUIRE_THAT(Buf[24000], Catch::Matchers::WithinAbs(-1.0f, 1e-3f));
	// Phase 0.25 at sample 12000 → output ≈ 0.
	REQUIRE_THAT(Buf[12000], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("FLfo: amount of 0 silences the output for any shape", "[lfo][amount]")
{
	for (uint8_t S = 0; S < static_cast<uint8_t>(ELfoShape::COUNT); ++S)
	{
		FLfo Lfo;
		Lfo.Prepare(48000.0);
		Lfo.SetParamValue(FLfo::Param_Shape, static_cast<float>(S));
		Lfo.SetParamValue(FLfo::Param_RateHz, 1.0f);
		Lfo.SetParamValue(FLfo::Param_Amount, 0.0f);

		Lfo.Process(Ctx());
		const float* Out = Lfo.GetOutputBuffer(0);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			REQUIRE(Out[I] == 0.0f);
		}
	}
}

TEST_CASE("FLfo: rate of 0 freezes the phase", "[lfo][rate]")
{
	FLfo Lfo;
	Lfo.Prepare(48000.0);
	Lfo.SetParamValue(FLfo::Param_Shape, static_cast<float>(ELfoShape::Sine));
	Lfo.SetParamValue(FLfo::Param_RateHz, 0.0f);
	Lfo.SetParamValue(FLfo::Param_Amount, 1.0f);

	// First block establishes a baseline; rate smoother converges to 0 over ~5ms.
	for (int B = 0; B < 200; ++B)
	{
		Lfo.Process(Ctx());
	}
	const float Last = Lfo.GetOutputBuffer(0)[BlockSize - 1];
	for (int B = 0; B < 50; ++B)
	{
		Lfo.Process(Ctx());
	}
	REQUIRE_THAT(Lfo.GetOutputBuffer(0)[BlockSize - 1], Catch::Matchers::WithinAbs(Last, 1e-5f));
}

TEST_CASE("FLfo: Sync rising edge resets phase to 0", "[lfo][sync]")
{
	FLfo Lfo;
	Lfo.Prepare(48000.0);
	Lfo.SetParamValue(FLfo::Param_Shape, static_cast<float>(ELfoShape::Saw));
	Lfo.SetParamValue(FLfo::Param_RateHz, 1.0f);
	Lfo.SetParamValue(FLfo::Param_Amount, 1.0f);

	// Run for a quarter period without sync — output should be ≈ -0.5 (saw mid-rise).
	for (int B = 0; B < 12000 / BlockSize; ++B)
	{
		Lfo.Process(Ctx());
	}

	// Now drive a single rising edge in the next block.
	std::vector<float> Sync(BlockSize, 0.0f);
	Sync[3] = 1.0f; // rising edge at sample 3
	Lfo.SetInputBuffer(FLfo::Input_Sync, Sync.data());
	Lfo.Process(Ctx());
	const float* Out = Lfo.GetOutputBuffer(0);

	// Right after the reset (sample 3 onwards) the saw restarts near -1.
	REQUIRE_THAT(Out[3], Catch::Matchers::WithinAbs(-1.0f, 1e-3f));
}

TEST_CASE("FLfo: Sync stays at 1 — only the rising edge resets, not the level", "[lfo][sync]")
{
	FLfo Lfo;
	Lfo.Prepare(48000.0);
	Lfo.SetParamValue(FLfo::Param_Shape, static_cast<float>(ELfoShape::Saw));
	Lfo.SetParamValue(FLfo::Param_RateHz, 1.0f);
	Lfo.SetParamValue(FLfo::Param_Amount, 1.0f);

	// First block has Sync high throughout — the LFO should reset only on the
	// initial rising edge (which we register here) and then advance normally.
	std::vector<float> Sync(BlockSize, 1.0f);
	Lfo.SetInputBuffer(FLfo::Input_Sync, Sync.data());
	Lfo.Process(Ctx());
	const float Last0 = Lfo.GetOutputBuffer(0)[BlockSize - 1];

	// Second block also Sync-high — no new rising edge; LFO continues from Last0.
	Lfo.Process(Ctx());
	const float* Out = Lfo.GetOutputBuffer(0);
	REQUIRE(Out[0] > Last0); // saw advancing
}
