#include "dsp/Svf.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace NodeSynth;

namespace
{
	struct FSvfRun
	{
		float LastLP = 0.0f;
		float LastHP = 0.0f;
		float LastBP = 0.0f;
		float PeakLPAbs = 0.0f;
		float PeakHPAbs = 0.0f;
		float PeakBPAbs = 0.0f;
		bool bAnyNaN = false;
	};

	FSvfRun RunSvf(float Cutoff, float Resonance, float InputDc, uint32_t Blocks = 500)
	{
		FSvf Svf;
		Svf.SetParamValue(FSvf::Param_Cutoff, Cutoff);
		Svf.SetParamValue(FSvf::Param_Resonance, Resonance);
		Svf.Prepare(48000.0);

		alignas(16) float AudioBuf[BlockSize];
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			AudioBuf[I] = InputDc;
		}
		Svf.SetInputBuffer(FSvf::Input_Audio, AudioBuf);
		Svf.SetInputBuffer(FSvf::Input_Cutoff, nullptr);
		Svf.SetInputBuffer(FSvf::Input_Resonance, nullptr);

		FProcessContext Ctx;
		Ctx.BlockSize = BlockSize;
		Ctx.SampleRate = 48000.0;

		FSvfRun Run;
		for (uint32_t B = 0; B < Blocks; ++B)
		{
			Svf.Process(Ctx);
			const float* LP = Svf.GetOutputBuffer(FSvf::Output_LowPass);
			const float* HP = Svf.GetOutputBuffer(FSvf::Output_HighPass);
			const float* BP = Svf.GetOutputBuffer(FSvf::Output_BandPass);
			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				if (std::isnan(LP[I]) || std::isnan(HP[I]) || std::isnan(BP[I]))
				{
					Run.bAnyNaN = true;
				}
				Run.PeakLPAbs = std::fmax(Run.PeakLPAbs, std::fabs(LP[I]));
				Run.PeakHPAbs = std::fmax(Run.PeakHPAbs, std::fabs(HP[I]));
				Run.PeakBPAbs = std::fmax(Run.PeakBPAbs, std::fabs(BP[I]));
			}
			Run.LastLP = LP[Ctx.BlockSize - 1];
			Run.LastHP = HP[Ctx.BlockSize - 1];
			Run.LastBP = BP[Ctx.BlockSize - 1];
		}
		return Run;
	}
}

TEST_CASE("SVF passes DC through LP and rejects DC on HP/BP", "[svf]")
{
	const FSvfRun Run = RunSvf(1000.0f, 0.2f, 1.0f);
	REQUIRE_FALSE(Run.bAnyNaN);
	// DC should sit on the LP output, away from HP/BP.
	REQUIRE_THAT(Run.LastLP, Catch::Matchers::WithinAbs(1.0f, 1e-2f));
	REQUIRE_THAT(Run.LastHP, Catch::Matchers::WithinAbs(0.0f, 1e-2f));
	REQUIRE_THAT(Run.LastBP, Catch::Matchers::WithinAbs(0.0f, 1e-2f));
}

TEST_CASE("SVF stays finite at resonance 1.0 (self-oscillation)", "[svf]")
{
	const FSvfRun Run = RunSvf(1000.0f, 1.0f, 0.0f);
	REQUIRE_FALSE(Run.bAnyNaN);
	// Self-oscillation at Q=inf grows without a damping term; bounded by the
	// trapezoidal integrator's stability but still noisy. Sanity: not explosive.
	REQUIRE(Run.PeakLPAbs < 1e6f);
	REQUIRE(Run.PeakHPAbs < 1e6f);
	REQUIRE(Run.PeakBPAbs < 1e6f);
}

TEST_CASE("SVF stays finite at extreme cutoffs", "[svf]")
{
	const FSvfRun Low = RunSvf(20.0f, 0.5f, 0.5f);
	const FSvfRun High = RunSvf(20000.0f, 0.5f, 0.5f);
	const FSvfRun OverTop = RunSvf(1e6f, 0.5f, 0.5f);  // clamped to 0.49 * SR
	REQUIRE_FALSE(Low.bAnyNaN);
	REQUIRE_FALSE(High.bAnyNaN);
	REQUIRE_FALSE(OverTop.bAnyNaN);
}

TEST_CASE("SVF with no audio input emits silence on all outputs", "[svf]")
{
	FSvf Svf;
	Svf.Prepare(48000.0);
	Svf.SetInputBuffer(FSvf::Input_Audio, nullptr);

	FProcessContext Ctx;
	Ctx.BlockSize = BlockSize;
	Ctx.SampleRate = 48000.0;
	Svf.Process(Ctx);

	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(Svf.GetOutputBuffer(FSvf::Output_LowPass)[I] == 0.0f);
		REQUIRE(Svf.GetOutputBuffer(FSvf::Output_HighPass)[I] == 0.0f);
		REQUIRE(Svf.GetOutputBuffer(FSvf::Output_BandPass)[I] == 0.0f);
	}
}
