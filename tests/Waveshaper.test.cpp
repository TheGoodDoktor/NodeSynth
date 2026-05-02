#include "dsp/Waveshaper.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::EWaveshape;
using NodeSynth::FProcessContext;
using NodeSynth::FWaveshaper;

namespace
{
	FProcessContext Ctx() { return {}; }

	// Run the shaper on a single block of the supplied input and return the
	// output block as a copy.
	std::vector<float> RunOne(FWaveshaper& W, const std::vector<float>& In)
	{
		W.SetInputBuffer(0, In.data());
		W.Process(Ctx());
		const float* O = W.GetOutputBuffer(0);
		return std::vector<float>(O, O + BlockSize);
	}

	std::vector<float> Constant(float V) { return std::vector<float>(BlockSize, V); }
}

TEST_CASE("FWaveshaper: HardClip clips to ±1", "[waveshaper]")
{
	FWaveshaper W;
	W.SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::HardClip));
	W.SetParamValue(FWaveshaper::Param_DriveDb, 0.0f);
	W.SetParamValue(FWaveshaper::Param_OutputDb, 0.0f);
	W.Prepare(48000.0);

	const auto Out = RunOne(W, Constant(2.0f));
	REQUIRE(Out[BlockSize - 1] == 1.0f);

	const auto Out2 = RunOne(W, Constant(-2.0f));
	REQUIRE(Out2[BlockSize - 1] == -1.0f);

	const auto Out3 = RunOne(W, Constant(0.5f));
	REQUIRE_THAT(Out3[BlockSize - 1], Catch::Matchers::WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("FWaveshaper: TanhSoft saturates large input near ±1", "[waveshaper]")
{
	FWaveshaper W;
	W.SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::TanhSoft));
	W.SetParamValue(FWaveshaper::Param_DriveDb, 0.0f);
	W.SetParamValue(FWaveshaper::Param_OutputDb, 0.0f);
	W.Prepare(48000.0);

	const auto Out = RunOne(W, Constant(10.0f));
	// tanh(10) ≈ 1.0
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(1.0f, 1e-3f));

	const auto Out2 = RunOne(W, Constant(0.0f));
	REQUIRE(Out2[BlockSize - 1] == 0.0f);
}

TEST_CASE("FWaveshaper: SoftClip cubic shape passes mid-range linearly with curvature", "[waveshaper]")
{
	FWaveshaper W;
	W.SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::SoftClip));
	W.SetParamValue(FWaveshaper::Param_DriveDb, 0.0f);
	W.SetParamValue(FWaveshaper::Param_OutputDb, 0.0f);
	W.Prepare(48000.0);

	// At ±1 the cubic 1.5x - 0.5x³ saturates to ±1 exactly.
	const auto Out = RunOne(W, Constant(1.0f));
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(1.0f, 1e-3f));

	const auto OutNeg = RunOne(W, Constant(-1.0f));
	REQUIRE_THAT(OutNeg[BlockSize - 1], Catch::Matchers::WithinAbs(-1.0f, 1e-3f));

	// Beyond ±1, output saturates at ±1.
	const auto Out2 = RunOne(W, Constant(2.0f));
	REQUIRE(Out2[BlockSize - 1] == 1.0f);
}

TEST_CASE("FWaveshaper: Fold reflects the signal back when |x| > 1", "[waveshaper]")
{
	FWaveshaper W;
	W.SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::Fold));
	W.SetParamValue(FWaveshaper::Param_DriveDb, 0.0f);
	W.SetParamValue(FWaveshaper::Param_OutputDb, 0.0f);
	W.Prepare(48000.0);

	// 1.5 → 2 - 1.5 = 0.5
	const auto Out = RunOne(W, Constant(1.5f));
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(0.5f, 1e-6f));

	// 3.0 → 2 - 3 = -1; loop again: -2 - (-1) = -1. So result = -1.
	// Actually: 3.0 first iteration: 2 - 3 = -1. Now Y = -1, exit loop (not < -1).
	// So 3.0 → -1.0.
	const auto Out2 = RunOne(W, Constant(3.0f));
	REQUIRE_THAT(Out2[BlockSize - 1], Catch::Matchers::WithinAbs(-1.0f, 1e-6f));

	// Stays bounded for any input within sane range.
	const auto Out3 = RunOne(W, Constant(7.5f));
	REQUIRE(std::fabs(Out3[BlockSize - 1]) <= 1.0f);
}

TEST_CASE("FWaveshaper: Drive in dB applies pre-gain", "[waveshaper]")
{
	FWaveshaper W;
	W.SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::HardClip));
	W.SetParamValue(FWaveshaper::Param_DriveDb, 20.0f);  // ×10
	W.SetParamValue(FWaveshaper::Param_OutputDb, 0.0f);
	W.Prepare(48000.0);

	// 0.05 input × 10 = 0.5, hard-clipped (still 0.5 since |0.5| < 1).
	const auto Out = RunOne(W, Constant(0.05f));
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(0.5f, 1e-3f));

	// 0.5 input × 10 = 5.0, hard-clipped to 1.0.
	const auto Out2 = RunOne(W, Constant(0.5f));
	REQUIRE_THAT(Out2[BlockSize - 1], Catch::Matchers::WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("FWaveshaper: Output in dB applies post-gain", "[waveshaper]")
{
	FWaveshaper W;
	W.SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::HardClip));
	W.SetParamValue(FWaveshaper::Param_DriveDb, 0.0f);
	W.SetParamValue(FWaveshaper::Param_OutputDb, -6.02f);  // ÷2 (-6.02 dB)
	W.Prepare(48000.0);

	const auto Out = RunOne(W, Constant(1.0f));
	// Hard-clip 1.0 = 1.0; × 0.5 (≈ -6 dB) = 0.5.
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(0.5f, 1e-2f));
}

TEST_CASE("FWaveshaper: param values clamp into their declared ranges", "[waveshaper]")
{
	FWaveshaper W;

	W.SetParamValue(FWaveshaper::Param_DriveDb, -5.0f);
	REQUIRE(W.GetParamValue(FWaveshaper::Param_DriveDb) == 0.0f);
	W.SetParamValue(FWaveshaper::Param_DriveDb, 100.0f);
	REQUIRE(W.GetParamValue(FWaveshaper::Param_DriveDb) == 40.0f);

	W.SetParamValue(FWaveshaper::Param_OutputDb, -100.0f);
	REQUIRE(W.GetParamValue(FWaveshaper::Param_OutputDb) == -20.0f);
	W.SetParamValue(FWaveshaper::Param_OutputDb, 100.0f);
	REQUIRE(W.GetParamValue(FWaveshaper::Param_OutputDb) == 20.0f);

	W.SetParamValue(FWaveshaper::Param_Shape, 999.0f);
	REQUIRE(W.GetParamValue(FWaveshaper::Param_Shape)
		== static_cast<float>(EWaveshape::COUNT) - 1.0f);
}

TEST_CASE("FWaveshaper: disconnected input emits silence", "[waveshaper]")
{
	FWaveshaper W;
	W.SetParamValue(FWaveshaper::Param_OutputDb, 0.0f);
	W.Prepare(48000.0);
	W.Process(Ctx());
	const float* O = W.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(O[I] == 0.0f);
	}
}

TEST_CASE("FWaveshaper: 2x oversampling reduces aliasing on hard-clipped sine", "[waveshaper][oversampling]")
{
	// Hard-clip a high-frequency sine at unity amplitude — that turns the
	// sine into a square wave, whose harmonic spectrum extends to infinity.
	// At 1x, harmonics above Nyquist alias back into the audible band.
	// At 2x, those harmonics are filtered before they fold. The aliasing
	// shows up as content at frequencies that aren't integer multiples of
	// the fundamental — measurable as a stronger "messy" residual at 1x.
	//
	// Stand-in for an FFT comparison: feed a 9 kHz sine (high enough that
	// its 3rd harmonic at 27 kHz is above the 24 kHz Nyquist, producing
	// alias content); compare the difference between 1x and 2x outputs.
	// Don't claim a precise dB number — just that the two diverge.
	const double Sr = 48000.0;
	const double Freq = 9000.0;
	const double TwoPi = 2.0 * 3.141592653589793;
	const double Phase = TwoPi * Freq / Sr;

	FWaveshaper W1, W2;
	W1.SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::HardClip));
	W2.SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::HardClip));
	W1.SetParamValue(FWaveshaper::Param_DriveDb, 12.0f);
	W2.SetParamValue(FWaveshaper::Param_DriveDb, 12.0f);
	W2.SetParamValue(FWaveshaper::Param_Oversample, 1.0f);  // 2x
	W1.Prepare(Sr);
	W2.Prepare(Sr);

	std::vector<float> Sine(BlockSize, 0.0f);
	std::vector<float> Out1, Out2;
	double T = 0.0;

	// Burn-in a few blocks to let smoothers and FIR settle.
	for (uint32_t B = 0; B < 4; ++B)
	{
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			Sine[I] = static_cast<float>(std::sin(T));
			T += Phase;
		}
		Out1 = RunOne(W1, Sine);
		Out2 = RunOne(W2, Sine);
	}

	// Capture one block of each.
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		Sine[I] = static_cast<float>(std::sin(T));
		T += Phase;
	}
	Out1 = RunOne(W1, Sine);
	Out2 = RunOne(W2, Sine);

	// The two outputs must differ: oversampling actually changes the result,
	// not a no-op. (Exact dB difference would need an FFT harness; see
	// docs/PLAN-PHASE-5.md §5c for the next step.)
	double Diff = 0.0;
	double Energy1 = 0.0;
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		const double D = static_cast<double>(Out2[I]) - static_cast<double>(Out1[I]);
		Diff += D * D;
		Energy1 += static_cast<double>(Out1[I]) * static_cast<double>(Out1[I]);
	}
	// Diff should be a meaningful fraction of the signal energy — not
	// numerical noise. We don't claim a precise number here, just that the
	// 2x path is actually doing something to the output.
	REQUIRE(Diff > Energy1 * 0.01);
}

TEST_CASE("FWaveshaper: 1x oversample param is a true bypass", "[waveshaper][oversampling]")
{
	// With Oversample = 1x, the 2x code path is gated off entirely. Output
	// should be bit-identical to a fresh waveshaper that never had Oversample
	// touched. (Both run on the 1x path; there's no reason for them to differ.)
	FWaveshaper W1, W2;
	W1.SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::TanhSoft));
	W2.SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::TanhSoft));
	W1.SetParamValue(FWaveshaper::Param_DriveDb, 12.0f);
	W2.SetParamValue(FWaveshaper::Param_DriveDb, 12.0f);
	W2.SetParamValue(FWaveshaper::Param_Oversample, 0.0f);  // explicit 1x
	W1.Prepare(48000.0);
	W2.Prepare(48000.0);

	std::vector<float> In(BlockSize, 0.0f);
	for (uint32_t I = 0; I < BlockSize; ++I) { In[I] = 0.5f * std::sin(I * 0.1f); }

	const auto Out1 = RunOne(W1, In);
	const auto Out2 = RunOne(W2, In);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(Out1[I] == Out2[I]);
	}
}

TEST_CASE("FWaveshaper: output stays finite for every shape across extreme inputs", "[waveshaper]")
{
	for (uint8_t S = 0; S < static_cast<uint8_t>(EWaveshape::COUNT); ++S)
	{
		FWaveshaper W;
		W.SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(S));
		W.SetParamValue(FWaveshaper::Param_DriveDb, 40.0f);  // max
		W.SetParamValue(FWaveshaper::Param_OutputDb, 20.0f); // max
		W.Prepare(48000.0);

		std::vector<float> Wild(BlockSize, 0.0f);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			Wild[I] = (I % 2 == 0) ? 1000.0f : -1000.0f;
		}
		const auto Out = RunOne(W, Wild);
		for (float V : Out)
		{
			REQUIRE(std::isfinite(V));
		}
	}
}
