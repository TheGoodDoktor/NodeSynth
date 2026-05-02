#include "dsp/Chorus.h"
#include "dsp/Flanger.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FChorus;
using NodeSynth::FFlanger;
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

TEST_CASE("FChorus: declares stereo output", "[chorus]")
{
	FChorus C;
	REQUIRE(C.IsOutputStereo(0));
}

TEST_CASE("FChorus: Mix=0 produces dry passthrough on the L channel", "[chorus]")
{
	FChorus C;
	C.SetParamValue(FChorus::Param_Mix, 0.0f);
	C.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0);
	C.SetInputBuffer(0, In.data(), 0);
	C.SetInputBuffer(0, In.data(), 1);
	C.Process(Ctx());
	const float* OL = C.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-4f));
	}
}

TEST_CASE("FChorus: produces stereo divergence for a mono input", "[chorus][stereo]")
{
	// L and R LFOs run 90° apart, so even an identical mono input produces
	// distinct L and R outputs once the LFO sweeps the delay line.
	FChorus C;
	C.SetParamValue(FChorus::Param_Mix, 1.0f);     // pure wet so the L/R difference is clear
	C.SetParamValue(FChorus::Param_Depth, 1.0f);   // full sweep
	C.SetParamValue(FChorus::Param_Rate, 1.0f);    // 1 Hz — slow
	C.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0);
	C.SetInputBuffer(0, In.data(), 0);
	C.SetInputBuffer(0, In.data(), 1);

	// Burn-in for the LFO to move past the initial 90° offset and the delay
	// lines to fill.
	std::vector<float> Quiet(BlockSize, 0.0f);
	for (uint32_t B = 0; B < 200; ++B)
	{
		C.Process(Ctx());
	}

	// Capture one block.
	C.Process(Ctx());
	const float* OL = C.GetOutputBuffer(0, 0);
	const float* OR = C.GetOutputBuffer(0, 1);

	double Diff = 0.0;
	for (uint32_t I = 0; I < BlockSize; ++I) { Diff += std::fabs(OL[I] - OR[I]); }
	REQUIRE(Diff > 0.001);
}

TEST_CASE("FFlanger: declares stereo output", "[flanger]")
{
	FFlanger F;
	REQUIRE(F.IsOutputStereo(0));
}

TEST_CASE("FFlanger: Mix=0, Feedback=0 produces dry passthrough", "[flanger]")
{
	FFlanger F;
	F.SetParamValue(FFlanger::Param_Mix, 0.0f);
	F.SetParamValue(FFlanger::Param_Feedback, 0.0f);
	F.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0);
	F.SetInputBuffer(0, In.data(), 0);
	F.SetInputBuffer(0, In.data(), 1);
	F.Process(Ctx());
	const float* OL = F.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-4f));
	}
}

TEST_CASE("FFlanger: feedback creates a sustained tail after impulse", "[flanger]")
{
	// Impulse on a flanger with high feedback should produce a tail that
	// decays slowly. With Feedback=0 the output should follow the impulse
	// and decay quickly.
	FFlanger F;
	F.SetParamValue(FFlanger::Param_Mix, 1.0f);
	F.SetParamValue(FFlanger::Param_Depth, 0.0f);   // no LFO sweep — fixed delay
	F.SetParamValue(FFlanger::Param_Feedback, 0.85f);
	F.Prepare(48000.0);

	std::vector<float> Impulse(BlockSize, 0.0f);
	Impulse[0] = 1.0f;
	F.SetInputBuffer(0, Impulse.data(), 0);
	F.SetInputBuffer(0, Impulse.data(), 1);
	F.Process(Ctx());

	// Run silent input for many blocks; capture peak energy across the tail.
	std::vector<float> Quiet(BlockSize, 0.0f);
	F.SetInputBuffer(0, Quiet.data(), 0);
	F.SetInputBuffer(0, Quiet.data(), 1);

	double Energy = 0.0;
	for (uint32_t B = 0; B < 50; ++B)
	{
		F.Process(Ctx());
		const float* O = F.GetOutputBuffer(0, 0);
		for (uint32_t I = 0; I < BlockSize; ++I) { Energy += O[I] * O[I]; }
	}
	REQUIRE(Energy > 0.001);  // tail is non-trivial
}

TEST_CASE("FFlanger: produces stereo divergence", "[flanger][stereo]")
{
	FFlanger F;
	F.SetParamValue(FFlanger::Param_Mix, 1.0f);
	F.SetParamValue(FFlanger::Param_Depth, 0.8f);
	F.SetParamValue(FFlanger::Param_Feedback, 0.4f);
	F.SetParamValue(FFlanger::Param_Rate, 0.5f);
	F.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 800.0, 48000.0);
	F.SetInputBuffer(0, In.data(), 0);
	F.SetInputBuffer(0, In.data(), 1);

	for (uint32_t B = 0; B < 100; ++B)
	{
		F.Process(Ctx());
	}
	F.Process(Ctx());
	const float* OL = F.GetOutputBuffer(0, 0);
	const float* OR = F.GetOutputBuffer(0, 1);

	double Diff = 0.0;
	for (uint32_t I = 0; I < BlockSize; ++I) { Diff += std::fabs(OL[I] - OR[I]); }
	REQUIRE(Diff > 0.001);
}
