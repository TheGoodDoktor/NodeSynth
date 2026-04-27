#include "dsp/Reverb.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FProcessContext;
using NodeSynth::FReverb;

namespace
{
	FProcessContext Ctx(double Sr = 48000.0)
	{
		FProcessContext C;
		C.BlockSize = BlockSize;
		C.SampleRate = Sr;
		return C;
	}

	// Run a fixed input through the reverb and collect the output. Audio source
	// is a single impulse at sample 0 if `bImpulse`, otherwise constant zero.
	std::vector<float> RunImpulse(FReverb& R, uint32_t NumBlocks)
	{
		std::vector<float> Out;
		Out.reserve(NumBlocks * BlockSize);
		std::vector<float> Block(BlockSize, 0.0f);
		Block[0] = 1.0f;  // impulse on the very first sample
		R.SetInputBuffer(0, Block.data());
		R.Process(Ctx());
		const float* O = R.GetOutputBuffer(0);
		for (uint32_t I = 0; I < BlockSize; ++I) { Out.push_back(O[I]); }

		std::vector<float> Quiet(BlockSize, 0.0f);
		for (uint32_t B = 1; B < NumBlocks; ++B)
		{
			R.SetInputBuffer(0, Quiet.data());
			R.Process(Ctx());
			for (uint32_t I = 0; I < BlockSize; ++I) { Out.push_back(O[I]); }
		}
		return Out;
	}

	// Peak absolute value over a window of the output buffer.
	float WindowPeak(const std::vector<float>& Buf, size_t Start, size_t End)
	{
		float Max = 0.0f;
		for (size_t I = Start; I < End && I < Buf.size(); ++I)
		{
			const float V = std::fabs(Buf[I]);
			if (V > Max) { Max = V; }
		}
		return Max;
	}
}

TEST_CASE("FReverb: Wet=0 produces dry passthrough", "[reverb]")
{
	FReverb R;
	R.SetParamValue(FReverb::Param_Wet, 0.0f);
	R.Prepare(48000.0);

	std::vector<float> Audio(BlockSize, 0.0f);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		Audio[I] = 0.5f * std::sin(I * 0.1f);
	}
	R.SetInputBuffer(0, Audio.data());
	R.Process(Ctx());
	const float* O = R.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(O[I], Catch::Matchers::WithinAbs(Audio[I], 1e-6f));
	}
}

TEST_CASE("FReverb: impulse decays toward silence", "[reverb]")
{
	FReverb R;
	R.SetParamValue(FReverb::Param_Wet, 1.0f);
	R.SetParamValue(FReverb::Param_RoomSize, 0.5f);
	R.SetParamValue(FReverb::Param_Damping, 0.5f);
	R.Prepare(48000.0);

	const auto Out = RunImpulse(R, 200);  // ~12800 samples = ~0.27 s

	// Early window has tail energy, late window should be much quieter.
	const float Early = WindowPeak(Out, 100, 2000);
	const float Late = WindowPeak(Out, 12000, 12700);
	REQUIRE(Early > 0.0f);
	REQUIRE(Late < Early * 0.5f);
}

TEST_CASE("FReverb: bigger RoomSize means longer tail", "[reverb]")
{
	const auto MeasureTail = [](float RoomSize) -> float
	{
		FReverb R;
		R.SetParamValue(FReverb::Param_Wet, 1.0f);
		R.SetParamValue(FReverb::Param_RoomSize, RoomSize);
		R.SetParamValue(FReverb::Param_Damping, 0.5f);
		R.Prepare(48000.0);
		const auto Out = RunImpulse(R, 300);
		// Tail energy: peak in a late window.
		return WindowPeak(Out, 15000, 19000);
	};

	const float SmallRoom = MeasureTail(0.2f);
	const float LargeRoom = MeasureTail(0.9f);
	REQUIRE(LargeRoom > SmallRoom);
}

TEST_CASE("FReverb: output stays bounded across param extremes", "[reverb]")
{
	const auto MaxOutputAtExtreme = [](float Room, float Damp, float Wet) -> float
	{
		FReverb R;
		R.SetParamValue(FReverb::Param_RoomSize, Room);
		R.SetParamValue(FReverb::Param_Damping, Damp);
		R.SetParamValue(FReverb::Param_Wet, Wet);
		R.Prepare(48000.0);
		const auto Out = RunImpulse(R, 200);
		float MaxAbs = 0.0f;
		for (float V : Out)
		{
			REQUIRE(std::isfinite(V));
			const float A = std::fabs(V);
			if (A > MaxAbs) { MaxAbs = A; }
		}
		return MaxAbs;
	};

	// Worst case for bound: max room, min damping. Output should remain finite
	// and well under 10× input (impulse = 1.0).
	REQUIRE(MaxOutputAtExtreme(1.0f, 0.0f, 1.0f) < 10.0f);
	REQUIRE(MaxOutputAtExtreme(0.0f, 1.0f, 1.0f) < 10.0f);
	REQUIRE(MaxOutputAtExtreme(0.5f, 0.5f, 1.0f) < 10.0f);
}

TEST_CASE("FReverb: param values clamp into [0, 1]", "[reverb]")
{
	FReverb R;
	R.SetParamValue(FReverb::Param_RoomSize, 5.0f);
	REQUIRE(R.GetParamValue(FReverb::Param_RoomSize) == 1.0f);

	R.SetParamValue(FReverb::Param_Damping, -1.0f);
	REQUIRE(R.GetParamValue(FReverb::Param_Damping) == 0.0f);

	R.SetParamValue(FReverb::Param_Wet, 0.42f);
	REQUIRE(R.GetParamValue(FReverb::Param_Wet) == 0.42f);
}

TEST_CASE("FReverb: scales tunings with sample rate", "[reverb]")
{
	// Smoke test that a 96 kHz Prepare allocates roughly twice as much delay
	// memory as 48 kHz (Freeverb tunings are at 44.1 kHz baseline). We don't
	// expose the comb sizes directly; instead measure Process behaviour after
	// Prepare to ensure no allocation / crash.
	FReverb R;
	R.SetParamValue(FReverb::Param_Wet, 1.0f);
	R.Prepare(96000.0);

	std::vector<float> Audio(BlockSize, 0.5f);
	for (int B = 0; B < 50; ++B)
	{
		R.SetInputBuffer(0, Audio.data());
		R.Process(Ctx(96000.0));
		const float* O = R.GetOutputBuffer(0);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			REQUIRE(std::isfinite(O[I]));
		}
	}
}

TEST_CASE("FReverb: disconnected audio input emits silence", "[reverb]")
{
	FReverb R;
	R.SetParamValue(FReverb::Param_Wet, 1.0f);
	R.Prepare(48000.0);
	for (int B = 0; B < 20; ++B)
	{
		R.Process(Ctx());
	}
	const float* O = R.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(O[I] == 0.0f);
	}
}
