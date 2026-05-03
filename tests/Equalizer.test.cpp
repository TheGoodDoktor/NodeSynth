#include "dsp/Biquad.h"
#include "dsp/DcBlocker.h"
#include "dsp/Equalizer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FBiquadCoeffs;
using NodeSynth::FBiquadState;
using NodeSynth::FDcBlocker;
using NodeSynth::FEqualizer;
using NodeSynth::FProcessContext;

namespace
{
	FProcessContext Ctx() { return {}; }

	std::vector<float> Sine(uint32_t N, double Freq, double Sr, float AmpLin = 1.0f)
	{
		std::vector<float> S(N, 0.0f);
		const double TwoPi = 2.0 * 3.141592653589793;
		double T = 0.0;
		const double Phase = TwoPi * Freq / Sr;
		for (uint32_t I = 0; I < N; ++I)
		{
			S[I] = AmpLin * static_cast<float>(std::sin(T));
			T += Phase;
		}
		return S;
	}

	float RmsOver(const float* Buf, uint32_t N)
	{
		double Sum = 0.0;
		for (uint32_t I = 0; I < N; ++I)
		{
			Sum += static_cast<double>(Buf[I]) * Buf[I];
		}
		return static_cast<float>(std::sqrt(Sum / N));
	}

	// Drive a biquad with a continuous sine at Freq; return the output's RMS
	// in the last block, after `BurnInBlocks` of settle. Phase is accumulated
	// across blocks so low frequencies (where one block holds <<1 cycle) stay
	// continuous — naively re-using a single block's sine introduces a step
	// discontinuity at every block boundary that the filter sees as broadband
	// content, swamping any tone-specific gain measurement.
	float MeasureBiquadRmsAtFreq(FBiquadCoeffs& C, float Freq, float Sr, uint32_t BurnInBlocks = 50)
	{
		FBiquadState S;
		const double TwoPi = 2.0 * 3.141592653589793;
		const double PhaseInc = TwoPi * Freq / Sr;
		double Phase = 0.0;
		float Out[BlockSize];
		for (uint32_t B = 0; B < BurnInBlocks; ++B)
		{
			for (uint32_t I = 0; I < BlockSize; ++I)
			{
				const float X = static_cast<float>(std::sin(Phase));
				Phase += PhaseInc;
				Out[I] = S.Process(C, X);
			}
		}
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			const float X = static_cast<float>(std::sin(Phase));
			Phase += PhaseInc;
			Out[I] = S.Process(C, X);
		}
		return RmsOver(Out, BlockSize);
	}
}

// ===== FBiquadCoeffs / FBiquadState =========================================

TEST_CASE("FBiquad: 0 dB low-shelf is unity passthrough", "[biquad]")
{
	FBiquadCoeffs C;
	C.SetLowShelf(/*Fc*/ 200.0f, /*Q*/ 0.7071f, /*DbGain*/ 0.0f, /*Sr*/ 48000.0f);
	FBiquadState S;
	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(S.Process(C, In[I]), Catch::Matchers::WithinAbs(In[I], 1e-4f));
	}
}

TEST_CASE("FBiquad: peak filter at +12 dB at fc boosts that frequency", "[biquad]")
{
	FBiquadCoeffs C;
	C.SetPeak(/*Fc*/ 1000.0f, /*Q*/ 1.0f, /*DbGain*/ +12.0f, /*Sr*/ 48000.0f);
	const float RmsAtFc = MeasureBiquadRmsAtFreq(C, 1000.0f, 48000.0f);
	const float RmsAtFar = MeasureBiquadRmsAtFreq(C, 100.0f, 48000.0f);
	// At fc the boost should bring the RMS well above the unity level (~0.707
	// for a unit-amplitude sine). The far-from-fc sine should be ~unity.
	REQUIRE(RmsAtFc > RmsAtFar * 1.5f);
	REQUIRE(RmsAtFc > 1.5f);
}

TEST_CASE("FBiquad: peak filter at -12 dB at fc cuts that frequency", "[biquad]")
{
	FBiquadCoeffs C;
	C.SetPeak(1000.0f, 1.0f, -12.0f, 48000.0f);
	const float RmsAtFc = MeasureBiquadRmsAtFreq(C, 1000.0f, 48000.0f);
	REQUIRE(RmsAtFc < 0.5f);  // below unity / 1.4
}

TEST_CASE("FBiquad: high-shelf reaches +12 dB asymptote at high frequency", "[biquad]")
{
	FBiquadCoeffs C;
	C.SetHighShelf(2000.0f, 0.7071f, 12.0f, 48000.0f);
	// Measure at well above corner — 12 kHz is 1 octave + 1.6 octaves above
	// 2 kHz, very close to the +12 dB asymptote (= 4× amplitude → RMS ~2.83
	// for a unit-amp sine).
	const float Rms = MeasureBiquadRmsAtFreq(C, 12000.0f, 48000.0f);
	REQUIRE(Rms > 2.5f);
}

TEST_CASE("FBiquad: low-shelf reaches +12 dB asymptote at low frequency", "[biquad]")
{
	FBiquadCoeffs C;
	C.SetLowShelf(2000.0f, 0.7071f, 12.0f, 48000.0f);
	const float Rms = MeasureBiquadRmsAtFreq(C, 50.0f, 48000.0f);
	REQUIRE(Rms > 2.5f);
}

// ===== FEqualizer ===========================================================

TEST_CASE("FEqualizer: declares stereo output", "[equalizer]")
{
	FEqualizer E;
	REQUIRE(E.IsOutputStereo(0));
}

TEST_CASE("FEqualizer: all gains at 0 dB → bit-identical passthrough", "[equalizer]")
{
	FEqualizer E;
	E.Prepare(48000.0);
	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, 0.5f);
	E.SetInputBuffer(0, In.data(), 0);
	E.SetInputBuffer(0, In.data(), 1);
	E.Process(Ctx());
	const float* OL = E.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-3f));
	}
}

TEST_CASE("FEqualizer: low-shelf boost increases low-frequency level", "[equalizer]")
{
	// Use frequencies that fit an integer number of cycles in BlockSize at
	// 48 kHz so the SetInputBuffer-and-replay test pattern stays continuous
	// (otherwise low frequencies get a step discontinuity per block that
	// swamps the per-tone gain measurement).
	FEqualizer E;
	E.SetParamValue(FEqualizer::Param_LowShelfFreq, 3000.0f);
	E.SetParamValue(FEqualizer::Param_LowShelfGainDb, 12.0f);
	E.Prepare(48000.0);

	// 750 Hz = 1 cycle/block; 12000 Hz = 16 cycles/block. Both block-clean.
	std::vector<float> InLow  = Sine(BlockSize,  750.0, 48000.0, 0.5f);
	std::vector<float> InHigh = Sine(BlockSize, 12000.0, 48000.0, 0.5f);

	E.SetInputBuffer(0, InLow.data(), 0);
	E.SetInputBuffer(0, InLow.data(), 1);
	for (uint32_t B = 0; B < 50; ++B) { E.Process(Ctx()); }
	E.Process(Ctx());
	const float RmsLow = RmsOver(E.GetOutputBuffer(0, 0), BlockSize);

	E.Prepare(48000.0);
	E.SetInputBuffer(0, InHigh.data(), 0);
	E.SetInputBuffer(0, InHigh.data(), 1);
	for (uint32_t B = 0; B < 50; ++B) { E.Process(Ctx()); }
	E.Process(Ctx());
	const float RmsHigh = RmsOver(E.GetOutputBuffer(0, 0), BlockSize);

	// Low (750 Hz) is below the 3 kHz corner → should be boosted.
	// High (12 kHz) is well above corner → should be near unity.
	REQUIRE(RmsLow > RmsHigh * 1.5f);
}

TEST_CASE("FEqualizer: high-shelf boost increases high-frequency level", "[equalizer]")
{
	FEqualizer E;
	E.SetParamValue(FEqualizer::Param_HighShelfFreq, 3000.0f);
	E.SetParamValue(FEqualizer::Param_HighShelfGainDb, 12.0f);
	E.Prepare(48000.0);

	std::vector<float> InLow  = Sine(BlockSize,  750.0, 48000.0, 0.5f);
	std::vector<float> InHigh = Sine(BlockSize, 12000.0, 48000.0, 0.5f);

	E.SetInputBuffer(0, InLow.data(), 0);
	E.SetInputBuffer(0, InLow.data(), 1);
	for (uint32_t B = 0; B < 50; ++B) { E.Process(Ctx()); }
	E.Process(Ctx());
	const float RmsLow = RmsOver(E.GetOutputBuffer(0, 0), BlockSize);

	E.Prepare(48000.0);
	E.SetInputBuffer(0, InHigh.data(), 0);
	E.SetInputBuffer(0, InHigh.data(), 1);
	for (uint32_t B = 0; B < 50; ++B) { E.Process(Ctx()); }
	E.Process(Ctx());
	const float RmsHigh = RmsOver(E.GetOutputBuffer(0, 0), BlockSize);

	REQUIRE(RmsHigh > RmsLow * 1.5f);
}

// ===== FDcBlocker ===========================================================

TEST_CASE("FDcBlocker: declares stereo output", "[dcblocker]")
{
	FDcBlocker D;
	REQUIRE(D.IsOutputStereo(0));
}

TEST_CASE("FDcBlocker: DC input decays toward zero", "[dcblocker]")
{
	FDcBlocker D;
	D.Prepare(48000.0);

	// Constant 1.0 input. The HP should decay the output toward 0 with TC
	// ~1/(2π·20 Hz) ≈ 8 ms. After ~50 ms, output should be << 0.1.
	std::vector<float> In(BlockSize, 1.0f);
	D.SetInputBuffer(0, In.data(), 0);
	D.SetInputBuffer(0, In.data(), 1);

	const uint32_t Blocks = static_cast<uint32_t>(48000.0 * 0.1) / BlockSize;  // 100 ms
	for (uint32_t B = 0; B < Blocks; ++B) { D.Process(Ctx()); }
	D.Process(Ctx());

	const float* OL = D.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(0.0f, 0.1f));
	}
}

TEST_CASE("FDcBlocker: 1 kHz sine passes through near-unchanged", "[dcblocker]")
{
	FDcBlocker D;
	D.Prepare(48000.0);
	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, 0.5f);
	D.SetInputBuffer(0, In.data(), 0);
	D.SetInputBuffer(0, In.data(), 1);

	for (uint32_t B = 0; B < 20; ++B) { D.Process(Ctx()); }
	D.Process(Ctx());

	const float InRms = RmsOver(In.data(), BlockSize);
	const float OutRms = RmsOver(D.GetOutputBuffer(0, 0), BlockSize);
	// Passband flat at 1 kHz — output RMS within ~5% of input.
	REQUIRE(OutRms > InRms * 0.95f);
	REQUIRE(OutRms < InRms * 1.05f);
}
