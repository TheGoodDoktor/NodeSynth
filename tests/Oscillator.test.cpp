#include "dsp/Oscillator.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace NodeSynth;

namespace
{
	// Drive the oscillator for N blocks at 48k and collect DC mean + peak abs.
	struct FOscStats
	{
		float Mean = 0.0f;
		float PeakAbs = 0.0f;
	};

	FOscStats RunOscillator(EOscShape S, float Frequency, uint32_t Blocks = 200)
	{
		FOscillator Osc;
		Osc.SetParamValue(FOscillator::Param_Shape, static_cast<float>(S));
		Osc.SetParamValue(FOscillator::Param_Frequency, Frequency);
		Osc.SetParamValue(FOscillator::Param_Amplitude, 1.0f);
		Osc.Prepare(48000.0);

		FProcessContext Ctx;
		Ctx.BlockSize = BlockSize;
		Ctx.SampleRate = 48000.0;

		double Sum = 0.0;
		float Peak = 0.0f;
		uint64_t SampleCount = 0;
		for (uint32_t B = 0; B < Blocks; ++B)
		{
			Osc.Process(Ctx);
			const float* Out = Osc.GetOutputBuffer(0);
			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				Sum += static_cast<double>(Out[I]);
				const float A = std::fabs(Out[I]);
				if (A > Peak)
				{
					Peak = A;
				}
			}
			SampleCount += Ctx.BlockSize;
		}

		FOscStats Stats;
		Stats.Mean = static_cast<float>(Sum / static_cast<double>(SampleCount));
		Stats.PeakAbs = Peak;
		return Stats;
	}
}

TEST_CASE("Sine oscillator has zero DC and unit peak", "[oscillator][sine]")
{
	// Skip the first several blocks of ramp-up by running long enough.
	const FOscStats Stats = RunOscillator(EOscShape::Sine, 440.0f, 500);
	REQUIRE_THAT(Stats.Mean, Catch::Matchers::WithinAbs(0.0f, 5e-3f));
	REQUIRE(Stats.PeakAbs <= 1.0f);
	REQUIRE(Stats.PeakAbs > 0.9f);
}

TEST_CASE("Saw oscillator has ~zero DC", "[oscillator][saw]")
{
	const FOscStats Stats = RunOscillator(EOscShape::Saw, 220.0f, 500);
	REQUIRE_THAT(Stats.Mean, Catch::Matchers::WithinAbs(0.0f, 2e-2f));
	REQUIRE(Stats.PeakAbs <= 1.01f);
}

TEST_CASE("Square oscillator has ~zero DC and unit peak", "[oscillator][square]")
{
	const FOscStats Stats = RunOscillator(EOscShape::Square, 220.0f, 500);
	REQUIRE_THAT(Stats.Mean, Catch::Matchers::WithinAbs(0.0f, 2e-2f));
	REQUIRE(Stats.PeakAbs <= 1.05f);
	REQUIRE(Stats.PeakAbs > 0.9f);
}

TEST_CASE("Triangle oscillator has ~zero DC and bounded amplitude", "[oscillator][triangle]")
{
	const FOscStats Stats = RunOscillator(EOscShape::Triangle, 220.0f, 500);
	REQUIRE_THAT(Stats.Mean, Catch::Matchers::WithinAbs(0.0f, 5e-2f));
	REQUIRE(Stats.PeakAbs <= 1.05f);
}

TEST_CASE("Noise oscillator fills the range", "[oscillator][noise]")
{
	const FOscStats Stats = RunOscillator(EOscShape::Noise, 440.0f, 500);
	REQUIRE_THAT(Stats.Mean, Catch::Matchers::WithinAbs(0.0f, 5e-2f));
	REQUIRE(Stats.PeakAbs > 0.5f);
	REQUIRE(Stats.PeakAbs <= 1.0f);
}

TEST_CASE("Shape param clamps to valid range", "[oscillator][params]")
{
	FOscillator Osc;
	Osc.SetParamValue(FOscillator::Param_Shape, -5.0f);
	REQUIRE(Osc.GetParamValue(FOscillator::Param_Shape) == 0.0f);
	Osc.SetParamValue(FOscillator::Param_Shape, 99.0f);
	REQUIRE(Osc.GetParamValue(FOscillator::Param_Shape) == static_cast<float>(EOscShape::COUNT) - 1);
}
