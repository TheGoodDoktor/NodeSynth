#include "dsp/Meter.h"
#include "dsp/Scope.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FMeter;
using NodeSynth::FProcessContext;
using NodeSynth::FScope;

namespace
{
	FProcessContext Ctx() { return {}; }
}

// -- FScope --------------------------------------------------------------------

TEST_CASE("FScope: passthrough copies input to output", "[scope]")
{
	FScope S;
	S.Prepare(48000.0);

	std::vector<float> In(BlockSize);
	for (uint32_t I = 0; I < BlockSize; ++I) { In[I] = std::sin(I * 0.1f); }
	S.SetInputBuffer(0, In.data());
	S.Process(Ctx());

	const float* Out = S.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(Out[I] == In[I]);
	}
}

TEST_CASE("FScope: snapshot returns the most recent samples", "[scope]")
{
	FScope S;
	S.Prepare(48000.0);

	// Push a known ramp through.
	std::vector<float> In(BlockSize, 0.0f);
	for (uint32_t B = 0; B < 5; ++B)
	{
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			In[I] = static_cast<float>(B * BlockSize + I);
		}
		S.SetInputBuffer(0, In.data());
		S.Process(Ctx());
	}

	// Last 5*BlockSize = 320 samples written; values are 0..319.
	std::vector<float> Snap(64, 0.0f);
	S.Snapshot(Snap.data(), 64);

	// The most recent 64 samples should be 256..319.
	for (size_t I = 0; I < 64; ++I)
	{
		REQUIRE_THAT(Snap[I], Catch::Matchers::WithinAbs(256.0f + I, 1e-6f));
	}
}

TEST_CASE("FScope: WindowSize param clamps to [32, Capacity]", "[scope]")
{
	FScope S;
	S.SetParamValue(FScope::Param_WindowSize, 0.0f);
	REQUIRE(S.GetParamValue(FScope::Param_WindowSize) == 32.0f);

	S.SetParamValue(FScope::Param_WindowSize, 99999.0f);
	REQUIRE(S.GetParamValue(FScope::Param_WindowSize)
		== static_cast<float>(FScope::Capacity));
}

TEST_CASE("FScope: ring wraps cleanly past Capacity", "[scope]")
{
	FScope S;
	S.Prepare(48000.0);

	// Push more than Capacity samples through. Last samples should still be
	// readable via Snapshot.
	std::vector<float> In(BlockSize, 0.0f);
	const uint32_t NumBlocks = (FScope::Capacity / BlockSize) * 3;
	uint64_t Counter = 0;
	for (uint32_t B = 0; B < NumBlocks; ++B)
	{
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			In[I] = static_cast<float>(Counter++);
		}
		S.SetInputBuffer(0, In.data());
		S.Process(Ctx());
	}
	// Counter is now NumBlocks * BlockSize. Last 64 samples should be the
	// preceding 64 values.
	std::vector<float> Snap(64, 0.0f);
	S.Snapshot(Snap.data(), 64);
	for (size_t I = 0; I < 64; ++I)
	{
		const float Expected = static_cast<float>(Counter - 64 + I);
		REQUIRE_THAT(Snap[I], Catch::Matchers::WithinAbs(Expected, 1e-6f));
	}
}

// -- FMeter --------------------------------------------------------------------

TEST_CASE("FMeter: passthrough copies input to output", "[meter]")
{
	FMeter M;
	M.Prepare(48000.0);

	std::vector<float> In(BlockSize, 0.5f);
	M.SetInputBuffer(0, In.data());
	M.Process(Ctx());
	const float* Out = M.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(Out[I] == 0.5f);
	}
}

TEST_CASE("FMeter: peak tracks the largest absolute sample", "[meter]")
{
	FMeter M;
	M.Prepare(48000.0);

	std::vector<float> In(BlockSize, 0.0f);
	In[10] = 0.7f;
	In[20] = -0.9f;
	In[30] = 0.3f;
	M.SetInputBuffer(0, In.data());
	M.Process(Ctx());
	REQUIRE_THAT(M.GetPeak(), Catch::Matchers::WithinAbs(0.9f, 1e-3f));
}

TEST_CASE("FMeter: peak holds, then decays", "[meter]")
{
	FMeter M;
	M.Prepare(48000.0);

	// Single big spike, then silence.
	std::vector<float> Spike(BlockSize, 0.0f);
	Spike[0] = 1.0f;
	M.SetInputBuffer(0, Spike.data());
	M.Process(Ctx());
	const float Initial = M.GetPeak();
	REQUIRE_THAT(Initial, Catch::Matchers::WithinAbs(1.0f, 1e-3f));

	std::vector<float> Quiet(BlockSize, 0.0f);
	M.SetInputBuffer(0, Quiet.data());
	// Inside the 500 ms hold window — peak should still be near 1.0.
	for (int B = 0; B < 5; ++B) { M.Process(Ctx()); }  // ~6.7 ms
	REQUIRE_THAT(M.GetPeak(), Catch::Matchers::WithinAbs(1.0f, 1e-2f));

	// Past the hold window — peak decays.
	for (int B = 0; B < 1500; ++B) { M.Process(Ctx()); }  // ~2 sec
	REQUIRE(M.GetPeak() < 0.5f);
}

TEST_CASE("FMeter: RMS of a sine wave converges to ~0.707 × amplitude", "[meter]")
{
	FMeter M;
	M.Prepare(48000.0);

	// Fill ~1 second of a 440 Hz sine at amplitude 1.0.
	std::vector<float> Sine(BlockSize, 0.0f);
	uint64_t Counter = 0;
	for (uint32_t B = 0; B < 48000 / BlockSize; ++B)
	{
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			const float T = static_cast<float>(Counter++) / 48000.0f;
			Sine[I] = std::sin(2.0f * 3.14159265f * 440.0f * T);
		}
		M.SetInputBuffer(0, Sine.data());
		M.Process(Ctx());
	}
	// RMS of unit-amplitude sine = 1/sqrt(2) ≈ 0.7071.
	REQUIRE_THAT(M.GetRms(), Catch::Matchers::WithinAbs(0.7071f, 0.05f));
}

TEST_CASE("FMeter: silence eventually reads as zero RMS", "[meter]")
{
	FMeter M;
	M.Prepare(48000.0);

	std::vector<float> Quiet(BlockSize, 0.0f);
	M.SetInputBuffer(0, Quiet.data());
	for (int B = 0; B < 500; ++B) { M.Process(Ctx()); }
	REQUIRE(M.GetRms() < 1e-3f);
}
