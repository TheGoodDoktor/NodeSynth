#include "audio/MicRing.h"
#include "dsp/MicInput.h"
#include "dsp/Node.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <thread>
#include <vector>

// NOTE: these tests never open a real capture device — device I/O (ma_device
// init/start) is manual-verified. FMicInput is constructed (which does NO
// miniaudio work) and driven through its ring via PushCapturedSamples, exactly
// as the capture callback would. RefreshDevices() / SetDevice() (the only
// paths that touch the audio backend) are not exercised here.

using NodeSynth::BlockSize;
using NodeSynth::FMicInput;
using NodeSynth::FMicRing;
using NodeSynth::FProcessContext;

namespace
{
	FProcessContext Ctx() { return {}; }
}

// ===== FMicRing (SPSC float ring) ===========================================

TEST_CASE("FMicRing: single-threaded push/pop preserves order", "[micinput][ring]")
{
	FMicRing Ring;
	REQUIRE(Ring.IsEmpty());
	REQUIRE(Ring.Push(1.0f));
	REQUIRE(Ring.Push(2.0f));
	REQUIRE(Ring.Available() == 2);

	float V = 0.0f;
	REQUIRE(Ring.Pop(V));
	REQUIRE(V == 1.0f);
	REQUIRE(Ring.Pop(V));
	REQUIRE(V == 2.0f);
	REQUIRE_FALSE(Ring.Pop(V));  // empty
	REQUIRE(Ring.IsEmpty());
}

TEST_CASE("FMicRing: concurrent producer/consumer loses nothing, stays in order", "[micinput][ring]")
{
	FMicRing Ring;
	constexpr uint32_t Count = 200000;

	std::atomic<bool> Start{ false };
	std::vector<float> Received;
	Received.reserve(Count);

	std::thread Consumer([&]()
	{
		while (!Start.load()) {}
		uint32_t Got = 0;
		float V = 0.0f;
		while (Got < Count)
		{
			if (Ring.Pop(V)) { Received.push_back(V); ++Got; }
		}
	});

	Start.store(true);
	for (uint32_t I = 0; I < Count; ++I)
	{
		// Spin until the slot frees up — the ring is far smaller than Count.
		while (!Ring.Push(static_cast<float>(I))) {}
	}
	Consumer.join();

	REQUIRE(Received.size() == Count);
	bool Ordered = true;
	for (uint32_t I = 0; I < Count; ++I)
	{
		if (Received[I] != static_cast<float>(I)) { Ordered = false; break; }
	}
	REQUIRE(Ordered);
}

// ===== FMicInput ============================================================

TEST_CASE("FMicInput: type, ports, and non-cloneable", "[micinput]")
{
	FMicInput Mic;
	REQUIRE(std::string(Mic.GetTypeName()) == "MicInput");
	REQUIRE(Mic.GetInputPorts().empty());
	REQUIRE(Mic.GetOutputPorts().size() == 1);
	REQUIRE_FALSE(Mic.IsOutputStereo(0));
	REQUIRE(Mic.Clone() == nullptr);
	// No enumeration at construction → device list is just "Off".
	REQUIRE(Mic.DeviceNames().size() == 1);
	REQUIRE(Mic.DeviceNames()[0] == "Off");
	REQUIRE_FALSE(Mic.IsCapturing());
}

TEST_CASE("FMicInput: Process drains the ring to the output (0 dB)", "[micinput]")
{
	FMicInput Mic;
	Mic.Prepare(48000.0);

	std::vector<float> In(BlockSize);
	for (uint32_t I = 0; I < BlockSize; ++I) { In[I] = 0.01f * static_cast<float>(I) - 0.3f; }
	Mic.PushCapturedSamples(In.data(), BlockSize);

	Mic.Process(Ctx());
	const float* Out = Mic.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(Out[I], Catch::Matchers::WithinAbs(In[I], 1e-5f));
	}
}

TEST_CASE("FMicInput: underrun yields silence, not garbage or a crash", "[micinput]")
{
	FMicInput Mic;
	Mic.Prepare(48000.0);
	// Ring is empty — Process must zero-fill.
	Mic.Process(Ctx());
	const float* Out = Mic.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(Out[I] == 0.0f);
	}
}

TEST_CASE("FMicInput: Gain param scales the captured signal", "[micinput]")
{
	FMicInput Mic;
	Mic.SetParamValue(FMicInput::Param_GainDb, 6.0f);  // set before Prepare → no glide
	Mic.Prepare(48000.0);

	std::vector<float> In(BlockSize, 1.0f);
	Mic.PushCapturedSamples(In.data(), BlockSize);
	Mic.Process(Ctx());

	const float* Out = Mic.GetOutputBuffer(0, 0);
	// +6 dB ≈ ×1.995.
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(1.995f, 0.02f));
}

TEST_CASE("FMicInput: overrun drops samples but never blocks or corrupts", "[micinput]")
{
	FMicInput Mic;
	Mic.Prepare(48000.0);

	// Push far more than the ring holds; PushCapturedSamples must return
	// promptly (drops the overflow) rather than spin or crash.
	std::vector<float> Flood(FMicRing::Capacity * 2, 0.5f);
	Mic.PushCapturedSamples(Flood.data(), static_cast<uint32_t>(Flood.size()));

	Mic.Process(Ctx());
	const float* Out = Mic.GetOutputBuffer(0, 0);
	// Whatever made it into the ring reads back as the pushed value.
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(Out[I], Catch::Matchers::WithinAbs(0.5f, 1e-5f));
	}
}
