#include "dsp/AutoPan.h"
#include "dsp/Tremolo.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FAutoPan;
using NodeSynth::FProcessContext;
using NodeSynth::FTremolo;

namespace
{
	FProcessContext Ctx() { return {}; }

	std::vector<float> Constant(float V)
	{
		return std::vector<float>(BlockSize, V);
	}
}

// ===== FTremolo =============================================================

TEST_CASE("FTremolo: declares stereo output", "[tremolo]")
{
	FTremolo T;
	REQUIRE(T.IsOutputStereo(0));
}

TEST_CASE("FTremolo: depth 0 is bit-identical passthrough", "[tremolo]")
{
	FTremolo T;
	T.SetParamValue(FTremolo::Param_Depth, 0.0f);
	T.Prepare(48000.0);

	std::vector<float> In(BlockSize, 0.0f);
	for (uint32_t I = 0; I < BlockSize; ++I) { In[I] = 0.5f * std::sin(I * 0.1f); }
	T.SetInputBuffer(0, In.data(), 0);
	T.SetInputBuffer(0, In.data(), 1);
	T.Process(Ctx());
	const float* OL = T.GetOutputBuffer(0, 0);
	const float* OR = T.GetOutputBuffer(0, 1);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-5f));
		REQUIRE_THAT(OR[I], Catch::Matchers::WithinAbs(In[I], 1e-5f));
	}
}

TEST_CASE("FTremolo: depth 1 ranges from 0 to input level over the LFO cycle", "[tremolo]")
{
	// Slow LFO + DC input: capture the gain at extremes by running for
	// long enough. Min should reach ~0; max should reach ~input level.
	FTremolo T;
	T.SetParamValue(FTremolo::Param_Depth, 1.0f);
	T.SetParamValue(FTremolo::Param_Rate, 1.0f);    // 1 Hz LFO — 1-second cycle
	T.SetParamValue(FTremolo::Param_Shape, 0.0f);   // Sine
	T.Prepare(48000.0);

	std::vector<float> In = Constant(1.0f);
	T.SetInputBuffer(0, In.data(), 0);
	T.SetInputBuffer(0, In.data(), 1);

	float MinSeen = 1.0f;
	float MaxSeen = 0.0f;
	const uint32_t Blocks = static_cast<uint32_t>(48000.0 / BlockSize);  // 1 second
	for (uint32_t B = 0; B < Blocks; ++B)
	{
		T.Process(Ctx());
		const float* OL = T.GetOutputBuffer(0, 0);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			if (OL[I] < MinSeen) { MinSeen = OL[I]; }
			if (OL[I] > MaxSeen) { MaxSeen = OL[I]; }
		}
	}
	REQUIRE(MinSeen < 0.05f);   // hits near zero at the LFO trough
	REQUIRE(MaxSeen > 0.95f);   // hits near input level at the LFO peak
}

TEST_CASE("FTremolo: Quad stereo mode produces L/R divergence", "[tremolo][stereo]")
{
	FTremolo T;
	T.SetParamValue(FTremolo::Param_Depth, 1.0f);
	T.SetParamValue(FTremolo::Param_Rate, 1.0f);
	T.SetParamValue(FTremolo::Param_Stereo, 1.0f);  // Quad
	T.Prepare(48000.0);

	std::vector<float> In = Constant(1.0f);
	T.SetInputBuffer(0, In.data(), 0);
	T.SetInputBuffer(0, In.data(), 1);

	double SumDiff = 0.0;
	const uint32_t Blocks = 50;
	for (uint32_t B = 0; B < Blocks; ++B)
	{
		T.Process(Ctx());
		const float* OL = T.GetOutputBuffer(0, 0);
		const float* OR = T.GetOutputBuffer(0, 1);
		for (uint32_t I = 0; I < BlockSize; ++I) { SumDiff += std::fabs(OL[I] - OR[I]); }
	}
	REQUIRE(SumDiff > 1.0);  // 180° offset → L and R visibly diverge
}

// ===== FAutoPan =============================================================

TEST_CASE("FAutoPan: declares stereo output", "[autopan]")
{
	FAutoPan A;
	REQUIRE(A.IsOutputStereo(0));
}

TEST_CASE("FAutoPan: depth 0 produces centred output (constant-power)", "[autopan]")
{
	// Constant-power centre = cos(π/4) = sin(π/4) ≈ 0.707. So a unit-DC
	// input should yield L = R ≈ 0.707.
	FAutoPan A;
	A.SetParamValue(FAutoPan::Param_Depth, 0.0f);
	A.Prepare(48000.0);

	std::vector<float> In = Constant(1.0f);
	A.SetInputBuffer(0, In.data(), 0);
	A.SetInputBuffer(0, In.data(), 1);
	A.Process(Ctx());
	const float* OL = A.GetOutputBuffer(0, 0);
	const float* OR = A.GetOutputBuffer(0, 1);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(0.7071f, 0.01f));
		REQUIRE_THAT(OR[I], Catch::Matchers::WithinAbs(0.7071f, 0.01f));
	}
}

TEST_CASE("FAutoPan: depth 1 sweeps fully L↔R over the LFO cycle", "[autopan]")
{
	FAutoPan A;
	A.SetParamValue(FAutoPan::Param_Depth, 1.0f);
	A.SetParamValue(FAutoPan::Param_Rate, 1.0f);
	A.Prepare(48000.0);

	std::vector<float> In = Constant(1.0f);
	A.SetInputBuffer(0, In.data(), 0);
	A.SetInputBuffer(0, In.data(), 1);

	float MaxL = 0.0f, MaxR = 0.0f;
	float MinL = 1.0f, MinR = 1.0f;
	const uint32_t Blocks = static_cast<uint32_t>(48000.0 / BlockSize);
	for (uint32_t B = 0; B < Blocks; ++B)
	{
		A.Process(Ctx());
		const float* OL = A.GetOutputBuffer(0, 0);
		const float* OR = A.GetOutputBuffer(0, 1);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			if (OL[I] > MaxL) { MaxL = OL[I]; }
			if (OL[I] < MinL) { MinL = OL[I]; }
			if (OR[I] > MaxR) { MaxR = OR[I]; }
			if (OR[I] < MinR) { MinR = OR[I]; }
		}
	}
	// L should reach near 1 (full left) and near 0 (full right). Same for R.
	REQUIRE(MaxL > 0.95f);
	REQUIRE(MinL < 0.05f);
	REQUIRE(MaxR > 0.95f);
	REQUIRE(MinR < 0.05f);
}

TEST_CASE("FAutoPan: constant-power invariant — L² + R² ≈ Input² always", "[autopan]")
{
	FAutoPan A;
	A.SetParamValue(FAutoPan::Param_Depth, 1.0f);
	A.SetParamValue(FAutoPan::Param_Rate, 2.0f);
	A.Prepare(48000.0);

	std::vector<float> In = Constant(1.0f);
	A.SetInputBuffer(0, In.data(), 0);
	A.SetInputBuffer(0, In.data(), 1);

	const uint32_t Blocks = 100;
	for (uint32_t B = 0; B < Blocks; ++B)
	{
		A.Process(Ctx());
		const float* OL = A.GetOutputBuffer(0, 0);
		const float* OR = A.GetOutputBuffer(0, 1);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			const float Power = OL[I] * OL[I] + OR[I] * OR[I];
			REQUIRE_THAT(Power, Catch::Matchers::WithinAbs(1.0f, 1e-3f));
		}
	}
}
