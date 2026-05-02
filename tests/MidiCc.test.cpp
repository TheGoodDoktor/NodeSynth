#include "dsp/MidiInput.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using NodeSynth::FMidiInput;

TEST_CASE("FMidiInput routes CC messages to the CC ring, not the note ring", "[midi][cc]")
{
	// Drive OnMidiMessage directly to bypass the RtMidi callback dependency
	// (no real MIDI device required for the test). Verify CC ($Bx) lands in
	// DrainCcEvents and note ($9x/$8x) doesn't.
	FMidiInput Mi;

	// CC#16 = 100 on channel 1 (status $B0).
	const unsigned char CcBytes[3] = { 0xB0, 16, 100 };
	Mi.OnMidiMessage(CcBytes, sizeof(CcBytes));

	// A note-on shouldn't appear in the CC drain.
	const unsigned char NoteOnBytes[3] = { 0x90, 60, 110 };
	Mi.OnMidiMessage(NoteOnBytes, sizeof(NoteOnBytes));

	std::vector<std::tuple<uint8_t, uint8_t, uint8_t>> Captured;
	Mi.DrainCcEvents([&](uint8_t Channel, uint8_t Cc, uint8_t Value)
	{
		Captured.emplace_back(Channel, Cc, Value);
	});

	REQUIRE(Captured.size() == 1);
	REQUIRE(std::get<0>(Captured[0]) == 1);    // channel 1 (status $B0 = channel 0 → 1-based)
	REQUIRE(std::get<1>(Captured[0]) == 16);
	REQUIRE(std::get<2>(Captured[0]) == 100);
}

TEST_CASE("FMidiInput preserves CC channel high nibble correctly", "[midi][cc]")
{
	// Channel 16 = status $BF (high nibble $B, low nibble $F = channel 15
	// 0-based, 16 1-based). Verify channel decode handles all 16 channels.
	FMidiInput Mi;
	for (uint8_t Ch = 0; Ch < 16; ++Ch)
	{
		const unsigned char B[3] = { static_cast<unsigned char>(0xB0 | Ch), 1, static_cast<unsigned char>(Ch * 8) };
		Mi.OnMidiMessage(B, sizeof(B));
	}
	std::vector<uint8_t> Channels;
	Mi.DrainCcEvents([&](uint8_t Channel, uint8_t /*Cc*/, uint8_t /*Value*/)
	{
		Channels.push_back(Channel);
	});
	REQUIRE(Channels.size() == 16);
	for (uint8_t I = 0; I < 16; ++I)
	{
		REQUIRE(Channels[I] == I + 1);
	}
}

TEST_CASE("FMidiInput drops sub-3-byte messages silently", "[midi][cc]")
{
	FMidiInput Mi;
	const unsigned char Short[1] = { 0xB0 };
	Mi.OnMidiMessage(Short, sizeof(Short));
	std::vector<uint8_t> Captured;
	Mi.DrainCcEvents([&](uint8_t, uint8_t, uint8_t Value)
	{
		Captured.push_back(Value);
	});
	REQUIRE(Captured.empty());
}
