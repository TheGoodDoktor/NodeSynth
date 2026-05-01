#include "dsp/Delay.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FDelay;
using NodeSynth::FProcessContext;

TEST_CASE("FDelay: stereo input produces stereo output through two independent lines", "[delay][stereo]")
{
	// Phase 5b.3 splits FDelay into two parallel delay lines. Same TimeMs /
	// Feedback / Tone params on both, so identical inputs produce identical
	// outputs — but distinct L and R inputs stay distinct end-to-end.
	FDelay Delay;
	Delay.SetParamValue(FDelay::Param_TimeMs, 50.0f);
	Delay.SetParamValue(FDelay::Param_Feedback, 0.4f);
	Delay.SetParamValue(FDelay::Param_Tone, 1.0f);
	Delay.Prepare(48000.0);

	REQUIRE(Delay.IsOutputStereo(0));

	std::vector<float> InL(BlockSize, 0.0f);
	std::vector<float> InR(BlockSize, 0.0f);
	InL[0] = 1.0f;     // impulse on L only
	InR[10] = 1.0f;    // impulse on R offset by 10 samples

	Delay.SetInputBuffer(FDelay::Input_Audio, InL.data(), 0);
	Delay.SetInputBuffer(FDelay::Input_Audio, InR.data(), 1);

	// 50 ms @ 48 kHz = 2400 samples = 38 blocks. Run a few blocks to let
	// the delayed impulse appear, then assert L and R differ.
	std::vector<float> OutL;
	std::vector<float> OutR;
	std::vector<float> Quiet(BlockSize, 0.0f);
	FProcessContext C;
	C.BlockSize = BlockSize;
	C.SampleRate = 48000.0;
	for (uint32_t B = 0; B < 50; ++B)
	{
		Delay.Process(C);
		const float* OL = Delay.GetOutputBuffer(0, 0);
		const float* OR = Delay.GetOutputBuffer(0, 1);
		REQUIRE(OL != OR);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			OutL.push_back(OL[I]);
			OutR.push_back(OR[I]);
		}
		Delay.SetInputBuffer(FDelay::Input_Audio, Quiet.data(), 0);
		Delay.SetInputBuffer(FDelay::Input_Audio, Quiet.data(), 1);
	}

	// Sum of absolute differences must be non-zero — the two impulses are
	// 10 samples apart on input and stay 10 samples apart through the delay.
	double TotalDiff = 0.0;
	for (size_t I = 0; I < OutL.size(); ++I)
	{
		TotalDiff += std::fabs(OutL[I] - OutR[I]);
	}
	REQUIRE(TotalDiff > 0.001);
}

namespace
{
	FProcessContext Ctx(double Sr = 48000.0)
	{
		FProcessContext C;
		C.BlockSize = BlockSize;
		C.SampleRate = Sr;
		return C;
	}

	// Run a fixed block of audio data through the delay (no Time control input)
	// and collect the full output. Convenience for delay-line tests.
	std::vector<float> Run(FDelay& Delay, const std::vector<float>& Audio, uint32_t NumBlocks)
	{
		std::vector<float> Out;
		Out.reserve(NumBlocks * BlockSize);
		std::vector<float> Block(BlockSize, 0.0f);
		size_t SrcCursor = 0;
		for (uint32_t B = 0; B < NumBlocks; ++B)
		{
			for (uint32_t I = 0; I < BlockSize; ++I)
			{
				Block[I] = (SrcCursor < Audio.size()) ? Audio[SrcCursor] : 0.0f;
				++SrcCursor;
			}
			Delay.SetInputBuffer(FDelay::Input_Audio, Block.data());
			Delay.Process(Ctx());
			const float* O = Delay.GetOutputBuffer(0);
			for (uint32_t I = 0; I < BlockSize; ++I)
			{
				Out.push_back(O[I]);
			}
		}
		return Out;
	}
}

TEST_CASE("FDelay: impulse delays by exactly TimeMs samples (zero feedback)", "[delay]")
{
	FDelay D;
	// Params set before Prepare so the time smoother is initialised to the
	// target value (mirrors patch-load order). Setting them after Prepare
	// would introduce a 5 ms glide that smears the impulse over a few samples.
	D.SetParamValue(FDelay::Param_TimeMs, 100.0f);  // 4800 samples
	D.SetParamValue(FDelay::Param_Feedback, 0.0f);
	D.SetParamValue(FDelay::Param_Tone, 1.0f);
	D.Prepare(48000.0);

	std::vector<float> Input(BlockSize * 100, 0.0f);
	Input[0] = 1.0f;  // single impulse at sample 0

	auto Out = Run(D, Input, 100);
	// Expect impulse at sample 4800.
	REQUIRE_THAT(Out[4800], Catch::Matchers::WithinAbs(1.0f, 1e-3f));
	REQUIRE_THAT(Out[4799], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
	REQUIRE_THAT(Out[4801], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
	REQUIRE_THAT(Out[0], Catch::Matchers::WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("FDelay: feedback at 0.5 produces decaying echoes", "[delay][feedback]")
{
	FDelay D;
	D.SetParamValue(FDelay::Param_TimeMs, 100.0f);  // 4800 samples
	D.SetParamValue(FDelay::Param_Feedback, 0.5f);
	D.SetParamValue(FDelay::Param_Tone, 1.0f);
	D.Prepare(48000.0);

	// Need at least 4 echoes' worth of audio: 19200 + a margin.
	const uint32_t Blocks = (20000 / BlockSize) + 1;
	std::vector<float> Input(BlockSize * Blocks, 0.0f);
	Input[0] = 1.0f;
	auto Out = Run(D, Input, Blocks);

	// Echo amplitudes: 1.0 at 4800, 0.5 at 9600, 0.25 at 14400, 0.125 at 19200.
	REQUIRE_THAT(Out[4800],  Catch::Matchers::WithinAbs(1.000f, 1e-3f));
	REQUIRE_THAT(Out[9600],  Catch::Matchers::WithinAbs(0.500f, 1e-3f));
	REQUIRE_THAT(Out[14400], Catch::Matchers::WithinAbs(0.250f, 1e-3f));
	REQUIRE_THAT(Out[19200], Catch::Matchers::WithinAbs(0.125f, 1e-3f));
}

TEST_CASE("FDelay: feedback >0.95 is clamped to prevent runaway", "[delay][feedback]")
{
	FDelay D;
	D.Prepare(48000.0);
	D.SetParamValue(FDelay::Param_Feedback, 5.0f);  // out of range
	REQUIRE(D.GetParamValue(FDelay::Param_Feedback) == 0.95f);

	D.SetParamValue(FDelay::Param_Feedback, -1.0f);
	REQUIRE(D.GetParamValue(FDelay::Param_Feedback) == 0.0f);
}

TEST_CASE("FDelay: TimeMs is clamped to [1, 2000]", "[delay]")
{
	FDelay D;
	D.SetParamValue(FDelay::Param_TimeMs, 0.0f);
	REQUIRE(D.GetParamValue(FDelay::Param_TimeMs) == 1.0f);

	D.SetParamValue(FDelay::Param_TimeMs, 9999.0f);
	REQUIRE(D.GetParamValue(FDelay::Param_TimeMs) == 2000.0f);
}

TEST_CASE("FDelay: disconnected input emits silence after the buffer is empty", "[delay]")
{
	FDelay D;
	D.Prepare(48000.0);
	D.SetParamValue(FDelay::Param_TimeMs, 50.0f);
	D.SetParamValue(FDelay::Param_Feedback, 0.0f);

	// No input wired — Process should produce all-zero output.
	for (int B = 0; B < 50; ++B)
	{
		FProcessContext C;
		D.Process(C);
	}
	const float* O = D.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(O[I] == 0.0f);
	}
}

TEST_CASE("FDelay: linear interpolation produces a partial sample for fractional delay", "[delay][interp]")
{
	FDelay D;
	// 100.01 ms × 48 samples/ms = 4800.48 samples — Floor=4800, Frac=0.48.
	// With an impulse at sample 0, samples 4800 and 4801 should each carry a
	// fraction of the impulse (linear interp).
	D.SetParamValue(FDelay::Param_TimeMs, 100.01f);
	D.SetParamValue(FDelay::Param_Feedback, 0.0f);
	D.SetParamValue(FDelay::Param_Tone, 1.0f);
	D.Prepare(48000.0);

	std::vector<float> Input(BlockSize * 110, 0.0f);
	Input[0] = 1.0f;
	auto Out = Run(D, Input, 110);

	// Sum of the interpolation pair should equal the full impulse energy.
	const float Pair = Out[4800] + Out[4801];
	REQUIRE_THAT(Pair, Catch::Matchers::WithinAbs(1.0f, 1e-3f));
	// Both samples should be non-zero (otherwise the delay snapped to integer).
	REQUIRE(Out[4800] > 0.1f);
	REQUIRE(Out[4801] > 0.1f);
}

TEST_CASE("FDelay: low Tone attenuates the second feedback echo more than the first", "[delay][tone]")
{
	// With Tone < 1, the LP filter's state takes a few samples to track the
	// impulse, so the *first* echo from the buffer won't be fully attenuated
	// (the feedback path adds the damped value which is still close to the
	// actual delayed sample). Subsequent echoes feed the damped signal back
	// into itself, compounding the attenuation. So later echoes should be
	// progressively further from the no-damping reference than earlier ones.
	const float NoDampEchoes[3] = { 1.0f, 0.5f, 0.25f };

	FDelay D;
	D.SetParamValue(FDelay::Param_TimeMs, 100.0f);
	D.SetParamValue(FDelay::Param_Feedback, 0.5f);
	D.SetParamValue(FDelay::Param_Tone, 0.05f);  // heavy LP
	D.Prepare(48000.0);

	std::vector<float> Input(BlockSize * 200, 0.0f);
	Input[0] = 1.0f;
	auto Out = Run(D, Input, 200);

	// Each echo should be lower than the un-damped reference. Compounding
	// attenuation: echo3 / NoDampEchoes[2] < echo2 / NoDampEchoes[1].
	const float Ratio2 = Out[9600] / NoDampEchoes[1];
	const float Ratio3 = Out[14400] / NoDampEchoes[2];
	REQUIRE(Ratio2 < 1.0f);
	REQUIRE(Ratio3 < Ratio2);
}

TEST_CASE("FDelay: Time Control input overrides the param when connected", "[delay][modulation]")
{
	FDelay D;
	D.SetParamValue(FDelay::Param_TimeMs, 50.0f);  // would normally be 2400 samples
	D.SetParamValue(FDelay::Param_Feedback, 0.0f);
	D.SetParamValue(FDelay::Param_Tone, 1.0f);
	D.Prepare(48000.0);

	// Drive Time Control input at constant 100 ms (4800 samples) — should
	// override the 50 ms param.
	std::vector<float> Audio(BlockSize, 0.0f);
	Audio[0] = 1.0f;
	std::vector<float> TimeBuf(BlockSize, 100.0f);

	D.SetInputBuffer(FDelay::Input_Audio, Audio.data());
	D.SetInputBuffer(FDelay::Input_TimeMs, TimeBuf.data());
	D.Process(Ctx());

	std::vector<float> Quiet(BlockSize, 0.0f);
	std::vector<float> TimeBuf2(BlockSize, 100.0f);
	std::vector<float> Out;
	Out.reserve(BlockSize * 100);
	for (uint32_t I = 0; I < BlockSize; ++I) { Out.push_back(D.GetOutputBuffer(0)[I]); }
	for (int B = 0; B < 100; ++B)
	{
		D.SetInputBuffer(FDelay::Input_Audio, Quiet.data());
		D.SetInputBuffer(FDelay::Input_TimeMs, TimeBuf2.data());
		D.Process(Ctx());
		for (uint32_t I = 0; I < BlockSize; ++I) { Out.push_back(D.GetOutputBuffer(0)[I]); }
	}

	// Expect impulse at sample 4800 (Control-driven), not 2400 (param).
	REQUIRE_THAT(Out[4800], Catch::Matchers::WithinAbs(1.0f, 1e-3f));
	REQUIRE_THAT(Out[2400], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("FDelay: buffer pointer is stable across Process calls (no allocation)", "[delay]")
{
	FDelay D;
	D.Prepare(48000.0);
	const float* BufferPtr = D.GetBufferData();
	const size_t Cap = D.GetCapacity();
	REQUIRE(BufferPtr != nullptr);
	REQUIRE(Cap > 0);

	std::vector<float> Audio(BlockSize, 0.0f);
	D.SetInputBuffer(FDelay::Input_Audio, Audio.data());
	for (int B = 0; B < 200; ++B) { D.Process(Ctx()); }

	REQUIRE(D.GetBufferData() == BufferPtr);
	REQUIRE(D.GetCapacity() == Cap);
}

TEST_CASE("FDelay: capacity is at least 2 seconds", "[delay]")
{
	FDelay D;
	D.Prepare(48000.0);
	REQUIRE(D.GetCapacity() >= 96000);

	D.Prepare(96000.0);
	REQUIRE(D.GetCapacity() >= 192000);
}
