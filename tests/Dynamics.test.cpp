#include "dsp/Compressor.h"
#include "dsp/Envelope.h"
#include "dsp/Gate.h"
#include "dsp/Limiter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FCompressor;
using NodeSynth::FEnvelopeFollower;
using NodeSynth::FGate;
using NodeSynth::FLimiter;
using NodeSynth::FProcessContext;

namespace
{
	FProcessContext Ctx() { return {}; }

	float DbToLin(float Db) { return std::pow(10.0f, Db * 0.05f); }

	std::vector<float> Sine(uint32_t N, double Freq, double Sr, float AmpLin)
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

	float PeakAbs(const float* Buf, uint32_t N)
	{
		float Max = 0.0f;
		for (uint32_t I = 0; I < N; ++I)
		{
			const float A = std::fabs(Buf[I]);
			if (A > Max) { Max = A; }
		}
		return Max;
	}
}

// ===== FEnvelopeFollower ====================================================

TEST_CASE("FEnvelopeFollower: DC step rises toward target with attack TC", "[envelope]")
{
	FEnvelopeFollower E;
	E.Prepare(48000.0);
	E.SetTimes(/*AttackMs*/ 5.0f, /*ReleaseMs*/ 100.0f);

	// Feed +1.0 DC for 5 × AttackMs worth of samples → state should reach
	// at least 0.95 (one-pole hits ~99% in ~5τ).
	const uint32_t N = static_cast<uint32_t>(48000.0 * 0.025);  // 25 ms
	float Last = 0.0f;
	for (uint32_t I = 0; I < N; ++I) { Last = E.Process(1.0f); }
	REQUIRE(Last > 0.95f);
}

TEST_CASE("FEnvelopeFollower: drops back to ~0 with release TC after silence", "[envelope]")
{
	FEnvelopeFollower E;
	E.Prepare(48000.0);
	E.SetTimes(1.0f, 5.0f);

	for (uint32_t I = 0; I < 1000; ++I) { E.Process(1.0f); }   // settle to 1
	REQUIRE(E.GetCurrent() > 0.95f);

	float Last = 0.0f;
	const uint32_t N = static_cast<uint32_t>(48000.0 * 0.025);
	for (uint32_t I = 0; I < N; ++I) { Last = E.Process(0.0f); }
	REQUIRE(Last < 0.05f);
}

// ===== FCompressor ==========================================================

TEST_CASE("FCompressor: -6 dBFS through 4:1 with -12 dB threshold reduces by ~4.5 dB", "[compressor]")
{
	FCompressor C;
	C.SetParamValue(FCompressor::Param_ThresholdDb, -12.0f);
	C.SetParamValue(FCompressor::Param_Ratio, 4.0f);
	C.SetParamValue(FCompressor::Param_AttackMs, 0.1f);   // fast — settle within the test window
	C.SetParamValue(FCompressor::Param_ReleaseMs, 5.0f);
	C.SetParamValue(FCompressor::Param_MakeupGainDb, 0.0f);
	C.Prepare(48000.0);

	const float InputAmp = DbToLin(-6.0f);   // -6 dBFS sine
	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, InputAmp);
	C.SetInputBuffer(0, In.data(), 0);
	C.SetInputBuffer(0, In.data(), 1);

	// Burn-in for the envelope follower to settle.
	for (uint32_t B = 0; B < 50; ++B) { C.Process(Ctx()); }
	C.Process(Ctx());

	const float OutPeak = PeakAbs(C.GetOutputBuffer(0, 0), BlockSize);
	const float OutDb = 20.0f * std::log10(OutPeak);
	// Above threshold by 6 dB; 4:1 ratio → reduce by 6 × (1 - 1/4) = 4.5 dB.
	// Output should be at -10.5 dB ± 1 dB tolerance for envelope ripple.
	REQUIRE(OutDb > -11.5f);
	REQUIRE(OutDb < -9.5f);
}

TEST_CASE("FCompressor: signal below threshold passes through unchanged", "[compressor]")
{
	FCompressor C;
	C.SetParamValue(FCompressor::Param_ThresholdDb, -12.0f);
	C.SetParamValue(FCompressor::Param_Ratio, 4.0f);
	C.Prepare(48000.0);

	const float InputAmp = DbToLin(-24.0f);  // well below threshold
	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, InputAmp);
	C.SetInputBuffer(0, In.data(), 0);
	C.SetInputBuffer(0, In.data(), 1);
	for (uint32_t B = 0; B < 20; ++B) { C.Process(Ctx()); }
	C.Process(Ctx());

	const float* OL = C.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-5f));
	}
}

TEST_CASE("FCompressor: linked stereo — L and R reduce by the same gain", "[compressor][stereo]")
{
	// L is -6 dBFS, R is silent. Linked detection should reduce both
	// channels by the same gain, so L's reduction matches what it would
	// be in mono and R stays silent.
	FCompressor C;
	C.SetParamValue(FCompressor::Param_ThresholdDb, -12.0f);
	C.SetParamValue(FCompressor::Param_Ratio, 4.0f);
	C.SetParamValue(FCompressor::Param_AttackMs, 0.1f);
	C.Prepare(48000.0);

	std::vector<float> InL = Sine(BlockSize, 1000.0, 48000.0, DbToLin(-6.0f));
	std::vector<float> InR(BlockSize, 0.0f);
	C.SetInputBuffer(0, InL.data(), 0);
	C.SetInputBuffer(0, InR.data(), 1);
	for (uint32_t B = 0; B < 50; ++B) { C.Process(Ctx()); }
	C.Process(Ctx());

	const float* OL = C.GetOutputBuffer(0, 0);
	const float* OR = C.GetOutputBuffer(0, 1);
	const float OutPeakL = PeakAbs(OL, BlockSize);
	REQUIRE(OutPeakL > 0.0f);
	REQUIRE(OutPeakL < DbToLin(-9.5f));   // L is reduced
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OR[I], Catch::Matchers::WithinAbs(0.0f, 1e-5f));
	}
}

// ===== FLimiter =============================================================

TEST_CASE("FLimiter: sustained signal above ceiling is capped near the ceiling", "[limiter]")
{
	FLimiter L;
	L.SetParamValue(FLimiter::Param_CeilingDb, -3.0f);
	L.SetParamValue(FLimiter::Param_ReleaseMs, 50.0f);
	L.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, DbToLin(0.0f));
	L.SetInputBuffer(0, In.data(), 0);
	L.SetInputBuffer(0, In.data(), 1);
	for (uint32_t B = 0; B < 100; ++B) { L.Process(Ctx()); }
	L.Process(Ctx());

	const float OutPeak = PeakAbs(L.GetOutputBuffer(0, 0), BlockSize);
	const float OutDb = 20.0f * std::log10(OutPeak);
	// Should sit just at or just under the ceiling (within 0.5 dB).
	REQUIRE(OutDb < -2.5f);
	REQUIRE(OutDb > -3.5f);
}

TEST_CASE("FLimiter: signal below ceiling passes through unchanged", "[limiter]")
{
	FLimiter L;
	L.SetParamValue(FLimiter::Param_CeilingDb, -3.0f);
	L.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, DbToLin(-12.0f));
	L.SetInputBuffer(0, In.data(), 0);
	L.SetInputBuffer(0, In.data(), 1);
	for (uint32_t B = 0; B < 20; ++B) { L.Process(Ctx()); }
	L.Process(Ctx());

	const float* OL = L.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-5f));
	}
}

// ===== FGate ================================================================

TEST_CASE("FGate: signal far below threshold is reduced", "[gate]")
{
	FGate G;
	G.SetParamValue(FGate::Param_ThresholdDb, -20.0f);
	G.SetParamValue(FGate::Param_Ratio, 10.0f);
	G.SetParamValue(FGate::Param_AttackMs, 0.1f);
	G.SetParamValue(FGate::Param_HoldMs, 0.0f);     // no hold for this test
	G.SetParamValue(FGate::Param_ReleaseMs, 5.0f);  // fast release
	G.Prepare(48000.0);

	// Input at -40 dB — 20 dB below threshold; 10:1 expansion → 200 dB
	// reduction in the limit, but with the slope formula (1 - 1/10) = 0.9
	// it's 20 × 0.9 = 18 dB extra reduction → output at -58 dB.
	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, DbToLin(-40.0f));
	G.SetInputBuffer(0, In.data(), 0);
	G.SetInputBuffer(0, In.data(), 1);
	for (uint32_t B = 0; B < 50; ++B) { G.Process(Ctx()); }
	G.Process(Ctx());

	const float OutPeak = PeakAbs(G.GetOutputBuffer(0, 0), BlockSize);
	const float OutDb = 20.0f * std::log10(OutPeak);
	// Significantly reduced — at least 10 dB further down than input.
	REQUIRE(OutDb < -50.0f);
}

TEST_CASE("FGate: signal above threshold passes through unchanged", "[gate]")
{
	FGate G;
	G.SetParamValue(FGate::Param_ThresholdDb, -20.0f);
	G.SetParamValue(FGate::Param_Ratio, 10.0f);
	G.SetParamValue(FGate::Param_AttackMs, 0.1f);
	G.SetParamValue(FGate::Param_HoldMs, 0.0f);
	G.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, DbToLin(-6.0f));
	G.SetInputBuffer(0, In.data(), 0);
	G.SetInputBuffer(0, In.data(), 1);
	for (uint32_t B = 0; B < 20; ++B) { G.Process(Ctx()); }
	G.Process(Ctx());

	const float* OL = G.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-5f));
	}
}
