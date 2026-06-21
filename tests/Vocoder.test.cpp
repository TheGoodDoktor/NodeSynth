#include "dsp/Biquad.h"
#include "dsp/Node.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using NodeSynth::BlockSize;
using NodeSynth::FBiquadCoeffs;
using NodeSynth::FBiquadState;

namespace
{
	float RmsOver(const float* Buf, uint32_t N)
	{
		double Sum = 0.0;
		for (uint32_t I = 0; I < N; ++I)
		{
			Sum += static_cast<double>(Buf[I]) * Buf[I];
		}
		return static_cast<float>(std::sqrt(Sum / N));
	}

	// Drive `Stages` cascaded biquads (all sharing C) with a continuous sine at
	// Freq; return the last block's output RMS after a settle period. Phase is
	// accumulated across blocks so low frequencies stay continuous — see the
	// note in Equalizer.test.cpp's MeasureBiquadRmsAtFreq.
	float MeasureBpfRms(const FBiquadCoeffs& C, float Freq, float Sr,
		uint32_t Stages = 1, uint32_t BurnInBlocks = 80)
	{
		FBiquadState S[4];
		const double TwoPi = 2.0 * 3.141592653589793;
		const double PhaseInc = TwoPi * Freq / Sr;
		double Phase = 0.0;
		float Out[BlockSize] = {};
		for (uint32_t B = 0; B < BurnInBlocks + 1; ++B)
		{
			for (uint32_t I = 0; I < BlockSize; ++I)
			{
				float X = static_cast<float>(std::sin(Phase));
				Phase += PhaseInc;
				for (uint32_t St = 0; St < Stages; ++St)
				{
					X = S[St].Process(C, X);
				}
				Out[I] = X;
			}
		}
		return RmsOver(Out, BlockSize);
	}
}

// ===== FBiquadCoeffs::SetBandpass (vocoder filter-bank primitive) ============

TEST_CASE("FBiquad bandpass: unity gain at centre frequency", "[vocoder][biquad]")
{
	FBiquadCoeffs C;
	C.SetBandpass(/*Fc*/ 1000.0f, /*Q*/ 4.0f, /*Sr*/ 48000.0f);
	// 0 dB peak gain → unit-amplitude sine at Fc passes at unity, RMS ≈ 0.707.
	const float RmsAtFc = MeasureBpfRms(C, 1000.0f, 48000.0f);
	REQUIRE_THAT(RmsAtFc, Catch::Matchers::WithinAbs(0.7071f, 0.03f));
}

TEST_CASE("FBiquad bandpass: strongly attenuates an octave either side", "[vocoder][biquad]")
{
	FBiquadCoeffs C;
	C.SetBandpass(1000.0f, 4.0f, 48000.0f);
	const float RmsAtFc   = MeasureBpfRms(C, 1000.0f, 48000.0f);
	const float RmsOctUp  = MeasureBpfRms(C, 2000.0f, 48000.0f);
	const float RmsOctDn  = MeasureBpfRms(C,  500.0f, 48000.0f);
	// A 2-pole BPF (Q=4) is ~16 dB down an octave away; assert at least 3×.
	REQUIRE(RmsAtFc > RmsOctUp * 3.0f);
	REQUIRE(RmsAtFc > RmsOctDn * 3.0f);
}

TEST_CASE("FBiquad bandpass: two cascaded stages keep unity at Fc, steeper skirts", "[vocoder][biquad]")
{
	FBiquadCoeffs C;
	C.SetBandpass(1000.0f, 4.0f, 48000.0f);

	// Each stage is unity at Fc, so a 2-stage cascade is still ≈ unity at Fc.
	const float CascAtFc = MeasureBpfRms(C, 1000.0f, 48000.0f, /*Stages*/ 2);
	REQUIRE_THAT(CascAtFc, Catch::Matchers::WithinAbs(0.7071f, 0.03f));

	// An octave away, the 4-pole cascade attenuates markedly more than the
	// single 2-pole section (the per-stage gain multiplies).
	const float SingleOctUp = MeasureBpfRms(C, 2000.0f, 48000.0f, /*Stages*/ 1);
	const float CascOctUp   = MeasureBpfRms(C, 2000.0f, 48000.0f, /*Stages*/ 2);
	REQUIRE(CascOctUp < SingleOctUp * 0.5f);
}

TEST_CASE("FBiquad bandpass: rejects DC and frequencies far below the band", "[vocoder][biquad]")
{
	FBiquadCoeffs C;
	C.SetBandpass(1000.0f, 4.0f, 48000.0f);

	// Constant (DC) input must decay to ~0 — a band-pass has zero DC gain.
	FBiquadState S;
	float Last = 0.0f;
	for (uint32_t I = 0; I < 8 * BlockSize; ++I)
	{
		Last = S.Process(C, 1.0f);
	}
	REQUIRE_THAT(Last, Catch::Matchers::WithinAbs(0.0f, 0.05f));

	// A tone two octaves below the band is heavily rejected.
	const float RmsFar = MeasureBpfRms(C, 250.0f, 48000.0f);
	REQUIRE(RmsFar < 0.1f);
}
