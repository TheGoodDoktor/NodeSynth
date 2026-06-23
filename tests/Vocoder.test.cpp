#include "dsp/Biquad.h"
#include "dsp/Node.h"
#include "dsp/Vocoder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FBiquadCoeffs;
using NodeSynth::FBiquadState;
using NodeSynth::FProcessContext;
using NodeSynth::FVocoder;

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

// ===== FVocoder =============================================================

namespace
{
	FProcessContext Ctx() { return {}; }

	// Deterministic broadband noise (fixed seed → reproducible tests).
	std::vector<float> Noise(uint32_t N, uint32_t Seed = 1234)
	{
		std::mt19937 Gen(Seed);
		std::uniform_real_distribution<float> Dist(-0.5f, 0.5f);
		std::vector<float> S(N);
		for (uint32_t I = 0; I < N; ++I) { S[I] = Dist(Gen); }
		return S;
	}

	std::vector<float> ContinuousSine(uint32_t N, double Freq, double Sr, float Amp = 0.5f)
	{
		std::vector<float> S(N);
		const double TwoPi = 2.0 * 3.141592653589793;
		const double Inc = TwoPi * Freq / Sr;
		double Phase = 0.0;
		for (uint32_t I = 0; I < N; ++I)
		{
			S[I] = Amp * static_cast<float>(std::sin(Phase));
			Phase += Inc;
		}
		return S;
	}

	float RmsVec(const std::vector<float>& V, uint32_t Start = 0)
	{
		double Sum = 0.0;
		const uint32_t N = static_cast<uint32_t>(V.size());
		for (uint32_t I = Start; I < N; ++I) { Sum += static_cast<double>(V[I]) * V[I]; }
		const uint32_t Count = (N > Start) ? (N - Start) : 1;
		return static_cast<float>(std::sqrt(Sum / Count));
	}

	// Energy of a signal inside a reference band-pass at Fc (Q=4, 4-pole) —
	// the same primitive the vocoder uses, run as a measuring probe.
	float BandEnergy(const std::vector<float>& Sig, float Fc, float Sr, uint32_t Start = 0)
	{
		FBiquadCoeffs C;
		C.SetBandpass(Fc, 4.0f, Sr);
		FBiquadState S0, S1;
		double Sum = 0.0;
		const uint32_t N = static_cast<uint32_t>(Sig.size());
		for (uint32_t I = 0; I < N; ++I)
		{
			const float Y = S1.Process(C, S0.Process(C, Sig[I]));
			if (I >= Start) { Sum += static_cast<double>(Y) * Y; }
		}
		const uint32_t Count = (N > Start) ? (N - Start) : 1;
		return static_cast<float>(std::sqrt(Sum / Count));
	}

	// Drive the vocoder block-by-block over equal-length carrier / modulator
	// sample vectors (mono, broadcast to both channels); collect output L.
	std::vector<float> RunVocoder(FVocoder& V,
		const std::vector<float>& Carrier, const std::vector<float>& Mod)
	{
		const uint32_t Total = static_cast<uint32_t>(Carrier.size());
		const uint32_t Blocks = Total / BlockSize;
		std::vector<float> Out;
		Out.reserve(Total);
		float CarBuf[BlockSize];
		float ModBuf[BlockSize];
		for (uint32_t B = 0; B < Blocks; ++B)
		{
			for (uint32_t I = 0; I < BlockSize; ++I)
			{
				CarBuf[I] = Carrier[B * BlockSize + I];
				ModBuf[I] = Mod[B * BlockSize + I];
			}
			V.SetInputBuffer(FVocoder::Input_Carrier, CarBuf, 0);
			V.SetInputBuffer(FVocoder::Input_Carrier, CarBuf, 1);
			V.SetInputBuffer(FVocoder::Input_Modulator, ModBuf, 0);
			V.SetInputBuffer(FVocoder::Input_Modulator, ModBuf, 1);
			V.Process(Ctx());
			const float* OL = V.GetOutputBuffer(0, 0);
			for (uint32_t I = 0; I < BlockSize; ++I) { Out.push_back(OL[I]); }
		}
		return Out;
	}
}

TEST_CASE("FVocoder: declares stereo output", "[vocoder]")
{
	FVocoder V;
	REQUIRE(V.IsOutputStereo(0));
}

TEST_CASE("FVocoder: silent modulator gates the carrier to silence", "[vocoder]")
{
	FVocoder V;
	V.Prepare(48000.0);  // Mix defaults to 1 (fully wet)

	const uint32_t N = 200 * BlockSize;
	std::vector<float> Carrier = Noise(N, 7);
	std::vector<float> Mod(N, 0.0f);  // dead modulator

	std::vector<float> Out = RunVocoder(V, Carrier, Mod);
	// No modulator energy → every band envelope sits at 0 → no carrier passes.
	REQUIRE(RmsVec(Out) < 1e-4f);
}

TEST_CASE("FVocoder: Mix=0 is exact dry carrier passthrough", "[vocoder]")
{
	FVocoder V;
	V.SetParamValue(FVocoder::Param_Mix, 0.0f);
	V.Prepare(48000.0);

	const uint32_t N = 4 * BlockSize;
	std::vector<float> Carrier = ContinuousSine(N, 440.0, 48000.0);
	std::vector<float> Mod = Noise(N, 99);

	std::vector<float> Out = RunVocoder(V, Carrier, Mod);
	for (uint32_t I = 0; I < N; ++I)
	{
		REQUIRE_THAT(Out[I], Catch::Matchers::WithinAbs(Carrier[I], 1e-5f));
	}
}

TEST_CASE("FVocoder: output energy tracks the modulator's spectral band", "[vocoder]")
{
	FVocoder V;
	V.Prepare(48000.0);  // 16 bands, formant 1.0, fully wet

	const uint32_t N = 400 * BlockSize;
	std::vector<float> Carrier = Noise(N, 3);                       // broadband carrier
	std::vector<float> Mod = ContinuousSine(N, 2000.0, 48000.0);    // tone at 2 kHz

	std::vector<float> Out = RunVocoder(V, Carrier, Mod);
	// Measure the back half (after the envelope followers settle).
	const uint32_t Settle = N / 2;
	const float Near = BandEnergy(Out, 2000.0f, 48000.0f, Settle);  // at the modulator tone
	const float Far  = BandEnergy(Out,  500.0f, 48000.0f, Settle);  // two octaves below
	REQUIRE(Near > Far * 2.0f);
}

TEST_CASE("FVocoder: formant shift moves the synthesised band upward", "[vocoder]")
{
	FVocoder V;
	V.SetParamValue(FVocoder::Param_Formant, 2.0f);  // shift carrier bands up an octave
	V.Prepare(48000.0);

	const uint32_t N = 400 * BlockSize;
	std::vector<float> Carrier = Noise(N, 5);
	std::vector<float> Mod = ContinuousSine(N, 1000.0, 48000.0);  // analysis energy at 1 kHz

	std::vector<float> Out = RunVocoder(V, Carrier, Mod);
	const uint32_t Settle = N / 2;
	// Analysis band ~1 kHz, synthesised at ~2 kHz (×Formant) → peak moves up.
	const float Up   = BandEnergy(Out, 2000.0f, 48000.0f, Settle);
	const float Base = BandEnergy(Out, 1000.0f, 48000.0f, Settle);
	REQUIRE(Up > Base * 1.5f);
}

TEST_CASE("FVocoder: all band counts produce non-silent output", "[vocoder]")
{
	for (float Idx : { 0.0f, 1.0f, 2.0f })  // 8 / 16 / 24 bands
	{
		FVocoder V;
		V.SetParamValue(FVocoder::Param_Bands, Idx);
		V.Prepare(48000.0);

		const uint32_t N = 100 * BlockSize;
		std::vector<float> Carrier = Noise(N, 11);
		std::vector<float> Mod = Noise(N, 22);

		std::vector<float> Out = RunVocoder(V, Carrier, Mod);
		REQUIRE(RmsVec(Out, N / 2) > 1e-3f);
	}
}

TEST_CASE("FVocoder: no allocation in Process (stable output pointers)", "[vocoder]")
{
	FVocoder V;
	V.Prepare(48000.0);
	const float* P0 = V.GetOutputBuffer(0, 0);
	std::vector<float> In(BlockSize, 0.25f);
	for (uint32_t B = 0; B < 50; ++B)
	{
		V.SetInputBuffer(FVocoder::Input_Carrier, In.data(), 0);
		V.SetInputBuffer(FVocoder::Input_Modulator, In.data(), 0);
		V.Process(Ctx());
	}
	REQUIRE(V.GetOutputBuffer(0, 0) == P0);
}

TEST_CASE("FVocoder: Clone round-trips all parameters", "[vocoder]")
{
	FVocoder V;
	V.SetParamValue(FVocoder::Param_Bands, 2.0f);
	V.SetParamValue(FVocoder::Param_Attack, 12.0f);
	V.SetParamValue(FVocoder::Param_Release, 200.0f);
	V.SetParamValue(FVocoder::Param_Formant, 1.5f);
	V.SetParamValue(FVocoder::Param_Mix, 0.6f);
	V.SetParamValue(FVocoder::Param_OutputDb, 6.0f);

	std::shared_ptr<NodeSynth::INode> C = V.Clone();
	REQUIRE(C != nullptr);
	REQUIRE_THAT(C->GetParamValue(FVocoder::Param_Bands),    Catch::Matchers::WithinAbs(2.0f, 1e-5f));
	REQUIRE_THAT(C->GetParamValue(FVocoder::Param_Attack),   Catch::Matchers::WithinAbs(12.0f, 1e-5f));
	REQUIRE_THAT(C->GetParamValue(FVocoder::Param_Release),  Catch::Matchers::WithinAbs(200.0f, 1e-5f));
	REQUIRE_THAT(C->GetParamValue(FVocoder::Param_Formant),  Catch::Matchers::WithinAbs(1.5f, 1e-5f));
	REQUIRE_THAT(C->GetParamValue(FVocoder::Param_Mix),      Catch::Matchers::WithinAbs(0.6f, 1e-5f));
	REQUIRE_THAT(C->GetParamValue(FVocoder::Param_OutputDb), Catch::Matchers::WithinAbs(6.0f, 1e-5f));
}
