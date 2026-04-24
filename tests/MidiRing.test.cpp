#include "midi/MidiRing.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using NodeSynth::FMidiEvent;
using NodeSynth::FMidiRing;

TEST_CASE("FMidiRing push/pop preserves FIFO order", "[midi][ring]")
{
	FMidiRing Ring;
	for (uint8_t I = 0; I < 10; ++I)
	{
		FMidiEvent E{ 0x90, I, static_cast<uint8_t>(100 + I) };
		REQUIRE(Ring.Push(E));
	}
	for (uint8_t I = 0; I < 10; ++I)
	{
		FMidiEvent Out;
		REQUIRE(Ring.Pop(Out));
		REQUIRE(Out.Data1 == I);
		REQUIRE(Out.Data2 == 100 + I);
	}
	REQUIRE(Ring.IsEmpty());
}

TEST_CASE("FMidiRing returns false when empty", "[midi][ring]")
{
	FMidiRing Ring;
	FMidiEvent Out;
	REQUIRE_FALSE(Ring.Pop(Out));
}

TEST_CASE("FMidiRing drops events when full", "[midi][ring]")
{
	FMidiRing Ring;
	FMidiEvent E{ 0x90, 60, 100 };
	// Capacity 256; ring holds Capacity-1 events before reporting full.
	size_t Pushed = 0;
	for (size_t I = 0; I < FMidiRing::Capacity + 10; ++I)
	{
		if (Ring.Push(E))
		{
			++Pushed;
		}
	}
	REQUIRE(Pushed == FMidiRing::Capacity - 1);
}

TEST_CASE("FMidiRing SPSC under concurrent producer/consumer", "[midi][ring]")
{
	FMidiRing Ring;
	constexpr int TotalEvents = 10000;
	std::atomic<bool> bDone{ false };
	std::vector<uint8_t> Received;
	Received.reserve(TotalEvents);

	std::thread Producer([&]
	{
		int Sent = 0;
		while (Sent < TotalEvents)
		{
			FMidiEvent E{ 0x90, static_cast<uint8_t>(Sent & 0x7F), 64 };
			if (Ring.Push(E))
			{
				++Sent;
			}
		}
		bDone.store(true);
	});

	while (!bDone.load() || !Ring.IsEmpty())
	{
		FMidiEvent E;
		if (Ring.Pop(E))
		{
			Received.push_back(E.Data1);
		}
	}
	Producer.join();

	REQUIRE(static_cast<int>(Received.size()) == TotalEvents);
	for (int I = 0; I < TotalEvents; ++I)
	{
		REQUIRE(Received[I] == static_cast<uint8_t>(I & 0x7F));
	}
}
