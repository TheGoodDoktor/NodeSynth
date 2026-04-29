#include "dsp/Mixer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FMixer;
using NodeSynth::FProcessContext;

namespace
{
	FProcessContext Ctx() { return {}; }

	std::vector<float> Constant(float V) { return std::vector<float>(BlockSize, V); }
}

TEST_CASE("FMixer: sums all four inputs at unity gain", "[mixer]")
{
	FMixer M;
	M.Prepare(48000.0);

	const auto A = Constant(0.1f);
	const auto B = Constant(0.2f);
	const auto C = Constant(0.3f);
	const auto D = Constant(0.4f);
	M.SetInputBuffer(0, A.data());
	M.SetInputBuffer(1, B.data());
	M.SetInputBuffer(2, C.data());
	M.SetInputBuffer(3, D.data());
	M.Process(Ctx());

	const float* Out = M.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(Out[I], Catch::Matchers::WithinAbs(1.0f, 1e-3f));
	}
}

TEST_CASE("FMixer: per-channel gain scales the contribution", "[mixer]")
{
	FMixer M;
	M.SetParamValue(FMixer::Param_Gain1, 0.5f);
	M.SetParamValue(FMixer::Param_Gain2, 1.0f);
	M.SetParamValue(FMixer::Param_Gain3, 0.0f);
	M.SetParamValue(FMixer::Param_Gain4, 2.0f);
	M.Prepare(48000.0);

	const auto A = Constant(1.0f);
	const auto B = Constant(1.0f);
	const auto C = Constant(1.0f);
	const auto D = Constant(1.0f);
	M.SetInputBuffer(0, A.data());
	M.SetInputBuffer(1, B.data());
	M.SetInputBuffer(2, C.data());
	M.SetInputBuffer(3, D.data());
	M.Process(Ctx());

	// Expected: 1.0*0.5 + 1.0*1.0 + 1.0*0.0 + 1.0*2.0 = 3.5
	const float* Out = M.GetOutputBuffer(0);
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(3.5f, 1e-3f));
}

TEST_CASE("FMixer: disconnected inputs read as silence", "[mixer]")
{
	FMixer M;
	M.Prepare(48000.0);

	// Only In1 connected.
	const auto A = Constant(0.5f);
	M.SetInputBuffer(0, A.data());
	M.Process(Ctx());

	const float* Out = M.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(Out[I], Catch::Matchers::WithinAbs(0.5f, 1e-3f));
	}
}

TEST_CASE("FMixer: gain clamps into [0, 2]", "[mixer]")
{
	FMixer M;
	M.SetParamValue(FMixer::Param_Gain1, -5.0f);
	REQUIRE(M.GetParamValue(FMixer::Param_Gain1) == 0.0f);
	M.SetParamValue(FMixer::Param_Gain2, 100.0f);
	REQUIRE(M.GetParamValue(FMixer::Param_Gain2) == 2.0f);
	M.SetParamValue(FMixer::Param_Gain3, 0.7f);
	REQUIRE(M.GetParamValue(FMixer::Param_Gain3) == 0.7f);
}

TEST_CASE("FMixer: silent when nothing is connected", "[mixer]")
{
	FMixer M;
	M.Prepare(48000.0);
	M.Process(Ctx());

	const float* Out = M.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(Out[I] == 0.0f);
	}
}

TEST_CASE("FMixer: clones cleanly with all gains preserved", "[mixer][clone]")
{
	FMixer Source;
	Source.SetParamValue(FMixer::Param_Gain1, 0.3f);
	Source.SetParamValue(FMixer::Param_Gain2, 0.6f);
	Source.SetParamValue(FMixer::Param_Gain3, 0.9f);
	Source.SetParamValue(FMixer::Param_Gain4, 1.2f);

	auto Cloned = Source.Clone();
	REQUIRE(Cloned != nullptr);
	REQUIRE_THAT(Cloned->GetParamValue(FMixer::Param_Gain1),
		Catch::Matchers::WithinAbs(0.3f, 1e-6f));
	REQUIRE_THAT(Cloned->GetParamValue(FMixer::Param_Gain4),
		Catch::Matchers::WithinAbs(1.2f, 1e-6f));
}
