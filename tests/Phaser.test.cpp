#include "dsp/Phaser.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FPhaser;
using NodeSynth::FProcessContext;

namespace
{
	FProcessContext Ctx() { return {}; }

	std::vector<float> Sine(uint32_t N, double Freq, double Sr)
	{
		std::vector<float> S(N, 0.0f);
		const double TwoPi = 2.0 * 3.141592653589793;
		double T = 0.0;
		const double Phase = TwoPi * Freq / Sr;
		for (uint32_t I = 0; I < N; ++I)
		{
			S[I] = static_cast<float>(std::sin(T));
			T += Phase;
		}
		return S;
	}
}

TEST_CASE("FPhaser: declares stereo output", "[phaser]")
{
	FPhaser P;
	REQUIRE(P.IsOutputStereo(0));
}

TEST_CASE("FPhaser: Mix=0 produces dry passthrough on the L channel", "[phaser]")
{
	FPhaser P;
	P.SetParamValue(FPhaser::Param_Mix, 0.0f);
	P.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0);
	P.SetInputBuffer(0, In.data(), 0);
	P.SetInputBuffer(0, In.data(), 1);
	P.Process(Ctx());
	const float* OL = P.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-4f));
	}
}

TEST_CASE("FPhaser: produces stereo divergence for a mono input", "[phaser][stereo]")
{
	// L and R LFOs run 90° apart, so identical mono input produces distinct
	// L and R outputs once the LFO has swept past its initial position.
	FPhaser P;
	P.SetParamValue(FPhaser::Param_Mix, 1.0f);
	P.SetParamValue(FPhaser::Param_Depth, 1.0f);
	P.SetParamValue(FPhaser::Param_Rate, 1.0f);
	P.SetParamValue(FPhaser::Param_Feedback, 0.0f);  // isolate the LFO offset effect
	P.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0);
	P.SetInputBuffer(0, In.data(), 0);
	P.SetInputBuffer(0, In.data(), 1);

	// Burn-in so the LFO sweeps past the initial 90° offset.
	for (uint32_t B = 0; B < 200; ++B)
	{
		P.Process(Ctx());
	}
	P.Process(Ctx());
	const float* OL = P.GetOutputBuffer(0, 0);
	const float* OR = P.GetOutputBuffer(0, 1);

	double Diff = 0.0;
	for (uint32_t I = 0; I < BlockSize; ++I) { Diff += std::fabs(OL[I] - OR[I]); }
	REQUIRE(Diff > 0.001);
}

TEST_CASE("FPhaser: feedback path keeps an impulse alive after silence", "[phaser]")
{
	// Mix=1, high feedback, impulse on input. After several silent input
	// blocks, the cascade's feedback tap should still be carrying energy.
	FPhaser P;
	P.SetParamValue(FPhaser::Param_Mix, 1.0f);
	P.SetParamValue(FPhaser::Param_Depth, 0.0f);     // static all-pass — keeps feedback path consistent
	P.SetParamValue(FPhaser::Param_Feedback, 0.85f);
	P.Prepare(48000.0);

	std::vector<float> Impulse(BlockSize, 0.0f);
	Impulse[0] = 1.0f;
	P.SetInputBuffer(0, Impulse.data(), 0);
	P.SetInputBuffer(0, Impulse.data(), 1);
	P.Process(Ctx());

	std::vector<float> Quiet(BlockSize, 0.0f);
	P.SetInputBuffer(0, Quiet.data(), 0);
	P.SetInputBuffer(0, Quiet.data(), 1);

	double Energy = 0.0;
	for (uint32_t B = 0; B < 20; ++B)
	{
		P.Process(Ctx());
		const float* O = P.GetOutputBuffer(0, 0);
		for (uint32_t I = 0; I < BlockSize; ++I) { Energy += O[I] * O[I]; }
	}
	REQUIRE(Energy > 0.0001);
}

TEST_CASE("FPhaser: every Stages choice produces non-silent output", "[phaser]")
{
	// Guards against accidentally bypassing all stages — at any stage count
	// the wet path should produce non-zero output for a sine input.
	for (int32_t StagesIdx = 0; StagesIdx < 3; ++StagesIdx)
	{
		FPhaser P;
		P.SetParamValue(FPhaser::Param_Mix, 1.0f);
		P.SetParamValue(FPhaser::Param_Depth, 0.5f);
		P.SetParamValue(FPhaser::Param_Stages, static_cast<float>(StagesIdx));
		P.Prepare(48000.0);

		std::vector<float> In = Sine(BlockSize, 800.0, 48000.0);
		P.SetInputBuffer(0, In.data(), 0);
		P.SetInputBuffer(0, In.data(), 1);
		// Burn-in so the LFO settles.
		for (uint32_t B = 0; B < 10; ++B) { P.Process(Ctx()); }
		P.Process(Ctx());

		const float* O = P.GetOutputBuffer(0, 0);
		double Energy = 0.0;
		for (uint32_t I = 0; I < BlockSize; ++I) { Energy += O[I] * O[I]; }
		REQUIRE(Energy > 0.0);
	}
}
