#include "dsp/Arpeggiator.h"

#include "graph/Graph.h"
#include "io/PatchSerializer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <filesystem>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FArpeggiator;
using NodeSynth::FProcessContext;

namespace
{
	FProcessContext Ctx() { return {}; }

	// Advance one step via a single external-clock rising edge (high at sample 0,
	// low after) and return the MIDI note emitted for that step.
	uint8_t PulseAndReadNote(FArpeggiator& Arp)
	{
		std::vector<float> Clock(BlockSize, 0.0f);
		Clock[0] = 1.0f;
		Arp.SetInputBuffer(FArpeggiator::Input_Clock, Clock.data());
		Arp.Process(Ctx());
		const float* N = Arp.GetOutputBuffer(FArpeggiator::Output_Note);
		return static_cast<uint8_t>(std::lround(N[BlockSize - 1]));
	}

	// Press a 3-note C major triad: C4(60), E4(64), G4(67), in pitch order.
	void PressTriad(FArpeggiator& Arp, float Vel = 1.0f)
	{
		Arp.HandleNoteOn(60, Vel);
		Arp.HandleNoteOn(64, Vel);
		Arp.HandleNoteOn(67, Vel);
	}
}

TEST_CASE("FArpeggiator: NoteOn / NoteOff track the held set", "[arp]")
{
	FArpeggiator Arp;
	Arp.Prepare(48000.0);
	REQUIRE(Arp.GetHeldCount() == 0);

	PressTriad(Arp);
	REQUIRE(Arp.GetHeldCount() == 3);

	// Duplicate NoteOn refreshes, doesn't grow the set.
	Arp.HandleNoteOn(64, 0.5f);
	REQUIRE(Arp.GetHeldCount() == 3);

	Arp.HandleNoteOff(64);
	REQUIRE(Arp.GetHeldCount() == 2);
	Arp.HandleNoteOff(60);
	Arp.HandleNoteOff(67);
	REQUIRE(Arp.GetHeldCount() == 0);
}

TEST_CASE("FArpeggiator: Up plays ascending, wraps at the top", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Pattern, static_cast<float>(FArpeggiator::Pattern_Up));
	Arp.Prepare(48000.0);
	PressTriad(Arp);

	REQUIRE(PulseAndReadNote(Arp) == 60);
	REQUIRE(PulseAndReadNote(Arp) == 64);
	REQUIRE(PulseAndReadNote(Arp) == 67);
	REQUIRE(PulseAndReadNote(Arp) == 60);  // wrap
}

TEST_CASE("FArpeggiator: Down plays descending", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Pattern, static_cast<float>(FArpeggiator::Pattern_Down));
	Arp.Prepare(48000.0);
	PressTriad(Arp);

	REQUIRE(PulseAndReadNote(Arp) == 67);
	REQUIRE(PulseAndReadNote(Arp) == 64);
	REQUIRE(PulseAndReadNote(Arp) == 60);
}

TEST_CASE("FArpeggiator: Up/Down turnaround doesn't repeat the endpoints", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Pattern, static_cast<float>(FArpeggiator::Pattern_UpDown));
	Arp.Prepare(48000.0);
	PressTriad(Arp);

	// Period = 2*3-2 = 4: 60, 64, 67, 64, then back to 60.
	REQUIRE(PulseAndReadNote(Arp) == 60);
	REQUIRE(PulseAndReadNote(Arp) == 64);
	REQUIRE(PulseAndReadNote(Arp) == 67);
	REQUIRE(PulseAndReadNote(Arp) == 64);
	REQUIRE(PulseAndReadNote(Arp) == 60);
}

TEST_CASE("FArpeggiator: As Played follows insertion order, not pitch", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Pattern, static_cast<float>(FArpeggiator::Pattern_AsPlayed));
	Arp.Prepare(48000.0);
	// Press out of pitch order: G, C, E.
	Arp.HandleNoteOn(67, 1.0f);
	Arp.HandleNoteOn(60, 1.0f);
	Arp.HandleNoteOn(64, 1.0f);

	REQUIRE(PulseAndReadNote(Arp) == 67);
	REQUIRE(PulseAndReadNote(Arp) == 60);
	REQUIRE(PulseAndReadNote(Arp) == 64);
}

TEST_CASE("FArpeggiator: octave range repeats the pattern transposed up", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Pattern, static_cast<float>(FArpeggiator::Pattern_Up));
	Arp.SetParamValue(FArpeggiator::Param_Octaves, 1.0f);  // index 1 = 2 octaves
	Arp.Prepare(48000.0);
	PressTriad(Arp);

	REQUIRE(Arp.GetResolvedLength() == 0);  // not built until first advance
	REQUIRE(PulseAndReadNote(Arp) == 60);
	REQUIRE(Arp.GetResolvedLength() == 6);
	REQUIRE(PulseAndReadNote(Arp) == 64);
	REQUIRE(PulseAndReadNote(Arp) == 67);
	REQUIRE(PulseAndReadNote(Arp) == 72);  // +12
	REQUIRE(PulseAndReadNote(Arp) == 76);
	REQUIRE(PulseAndReadNote(Arp) == 79);
	REQUIRE(PulseAndReadNote(Arp) == 60);  // wrap
}

TEST_CASE("FArpeggiator: octave transposition clamps to MIDI 127", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Octaves, 3.0f);  // 4 octaves
	Arp.Prepare(48000.0);
	Arp.HandleNoteOn(120, 1.0f);  // +36 would be 156 → clamps

	uint8_t Max = 0;
	for (int32_t I = 0; I < 4; ++I)
	{
		const uint8_t N = PulseAndReadNote(Arp);
		if (N > Max) { Max = N; }
	}
	REQUIRE(Max == 127);
}

TEST_CASE("FArpeggiator: a connected Clock overrides the internal clock", "[arp]")
{
	FArpeggiator Arp;
	Arp.Prepare(48000.0);
	PressTriad(Arp);

	// Clock input connected but held low → no edges → never advances.
	std::vector<float> Low(BlockSize, 0.0f);
	for (int32_t B = 0; B < 50; ++B)
	{
		Arp.SetInputBuffer(FArpeggiator::Input_Clock, Low.data());
		Arp.Process(Ctx());
	}
	REQUIRE(Arp.GetStepIndex() == -1);

	const float* G = Arp.GetOutputBuffer(FArpeggiator::Output_Gate);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(G[I] == 0.0f);
	}

	// A single edge advances exactly one step.
	REQUIRE(PulseAndReadNote(Arp) == 60);
	REQUIRE(Arp.GetStepIndex() == 0);
}

TEST_CASE("FArpeggiator: internal clock step length tracks BPM and Rate", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Bpm, 120.0f);
	Arp.SetParamValue(FArpeggiator::Param_Rate, 3.0f);  // 1/16 → 4 steps/beat
	Arp.Prepare(48000.0);
	PressTriad(Arp);

	// 120 BPM → 24000 samples/beat; 1/16 → 6000 samples/step.
	Arp.Process(Ctx());  // fresh-chord retrigger fires step 0
	REQUIRE(Arp.GetStepIndex() == 0);
	REQUIRE(Arp.GetSamplesPerStep() == 6000);
}

TEST_CASE("FArpeggiator: internal clock cycles steps over time", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Bpm, 120.0f);
	Arp.SetParamValue(FArpeggiator::Param_Rate, 3.0f);  // 6000 samples/step
	Arp.Prepare(48000.0);
	PressTriad(Arp);

	int32_t LastStep = -2;
	int32_t Changes = 0;
	const uint32_t NumBlocks = 48000 / BlockSize;  // 1 second
	for (uint32_t B = 0; B < NumBlocks; ++B)
	{
		Arp.Process(Ctx());
		const int32_t Now = Arp.GetStepIndex();
		if (Now != LastStep) { ++Changes; LastStep = Now; }
	}
	// ~8 steps/second at 6000 samples/step. Range covers boundary rounding.
	REQUIRE(Changes >= 6);
	REQUIRE(Changes <= 10);
}

TEST_CASE("FArpeggiator: gate is high for the gate-length fraction of a step", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Bpm, 120.0f);
	Arp.SetParamValue(FArpeggiator::Param_Rate, 3.0f);  // 6000 samples/step
	Arp.SetParamValue(FArpeggiator::Param_Gate, 0.5f);
	Arp.Prepare(48000.0);
	PressTriad(Arp);

	int32_t HighCount = 0;
	const uint32_t NumBlocks = 6000 / BlockSize;  // one step's worth
	for (uint32_t B = 0; B < NumBlocks; ++B)
	{
		Arp.Process(Ctx());
		const float* G = Arp.GetOutputBuffer(FArpeggiator::Output_Gate);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			if (G[I] > 0.5f) { ++HighCount; }
		}
	}
	// ~3000 of ~6000 samples high. Allow generous slack for block quantisation.
	REQUIRE(HighCount > 2400);
	REQUIRE(HighCount < 3600);
}

TEST_CASE("FArpeggiator: no notes held keeps the gate low", "[arp]")
{
	FArpeggiator Arp;
	Arp.Prepare(48000.0);

	for (int32_t B = 0; B < 200; ++B)
	{
		Arp.Process(Ctx());
		const float* G = Arp.GetOutputBuffer(FArpeggiator::Output_Gate);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			REQUIRE(G[I] == 0.0f);
		}
	}
}

TEST_CASE("FArpeggiator: releasing all keys drops the gate (non-latch)", "[arp]")
{
	FArpeggiator Arp;
	Arp.Prepare(48000.0);
	PressTriad(Arp);
	PulseAndReadNote(Arp);  // playing

	Arp.HandleNoteOff(60);
	Arp.HandleNoteOff(64);
	Arp.HandleNoteOff(67);
	REQUIRE(Arp.GetHeldCount() == 0);

	Arp.SetInputBuffer(FArpeggiator::Input_Clock, nullptr);
	Arp.Process(Ctx());
	const float* G = Arp.GetOutputBuffer(FArpeggiator::Output_Gate);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(G[I] == 0.0f);
	}
}

TEST_CASE("FArpeggiator: removing a note mid-run stays in range and in the held set", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Pattern, static_cast<float>(FArpeggiator::Pattern_Up));
	Arp.Prepare(48000.0);
	PressTriad(Arp);
	PulseAndReadNote(Arp);
	PulseAndReadNote(Arp);

	Arp.HandleNoteOff(64);  // remove the middle note
	REQUIRE(Arp.GetHeldCount() == 2);

	// Every subsequent step must be one of the remaining held notes.
	for (int32_t I = 0; I < 8; ++I)
	{
		const uint8_t N = PulseAndReadNote(Arp);
		REQUIRE((N == 60 || N == 67));
	}
}

TEST_CASE("FArpeggiator: latch keeps playing after release, new note starts a fresh phrase", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Pattern, static_cast<float>(FArpeggiator::Pattern_Up));
	Arp.SetParamValue(FArpeggiator::Param_Latch, 1.0f);
	Arp.Prepare(48000.0);
	PressTriad(Arp);

	REQUIRE(PulseAndReadNote(Arp) == 60);

	// Release everything — latch sustains the set.
	Arp.HandleNoteOff(60);
	Arp.HandleNoteOff(64);
	Arp.HandleNoteOff(67);
	REQUIRE(Arp.GetHeldCount() == 3);
	REQUIRE(PulseAndReadNote(Arp) == 64);  // pattern keeps running

	// A new key after full release clears the latched set and starts fresh.
	Arp.HandleNoteOn(72, 1.0f);
	REQUIRE(Arp.GetHeldCount() == 1);
	REQUIRE(PulseAndReadNote(Arp) == 72);
}

TEST_CASE("FArpeggiator: Random only emits notes from the held set", "[arp]")
{
	FArpeggiator Arp;
	Arp.SetParamValue(FArpeggiator::Param_Pattern, static_cast<float>(FArpeggiator::Pattern_Random));
	Arp.Prepare(48000.0);
	PressTriad(Arp);

	for (int32_t I = 0; I < 64; ++I)
	{
		const uint8_t N = PulseAndReadNote(Arp);
		REQUIRE((N == 60 || N == 64 || N == 67));
	}
}

TEST_CASE("FArpeggiator: velocity output reflects the pressed note's velocity", "[arp]")
{
	FArpeggiator Arp;
	Arp.Prepare(48000.0);
	Arp.HandleNoteOn(60, 0.4f);

	std::vector<float> Clock(BlockSize, 0.0f);
	Clock[0] = 1.0f;
	Arp.SetInputBuffer(FArpeggiator::Input_Clock, Clock.data());
	Arp.Process(Ctx());

	const float* V = Arp.GetOutputBuffer(FArpeggiator::Output_Velocity);
	REQUIRE_THAT(V[BlockSize - 1], Catch::Matchers::WithinAbs(0.4f, 0.001f));
}

TEST_CASE("FArpeggiator: Process does not allocate (output pointers stable)", "[arp]")
{
	FArpeggiator Arp;
	Arp.Prepare(48000.0);
	PressTriad(Arp);

	const float* G0 = Arp.GetOutputBuffer(FArpeggiator::Output_Gate);
	const float* F0 = Arp.GetOutputBuffer(FArpeggiator::Output_Frequency);
	for (int32_t B = 0; B < 500; ++B)
	{
		PulseAndReadNote(Arp);
	}
	REQUIRE(Arp.GetOutputBuffer(FArpeggiator::Output_Gate) == G0);
	REQUIRE(Arp.GetOutputBuffer(FArpeggiator::Output_Frequency) == F0);
}

TEST_CASE("FArpeggiator: Clone is rejected (polyphony source, not per-voice)", "[arp]")
{
	FArpeggiator Arp;
	REQUIRE(Arp.Clone() == nullptr);
}

#ifdef NODESYNTH_SOURCE_DIR
TEST_CASE("FArpeggiator: bundled Arp Pluck preset loads and compiles", "[arp][preset]")
{
	const std::filesystem::path Path =
		std::filesystem::path(NODESYNTH_SOURCE_DIR) / "presets" / "Sequenced" / "Arp Pluck.json";
	auto Loaded = NodeSynth::LoadPatch(Path);
	REQUIRE(Loaded.has_value());

	auto Snapshot = Loaded->Model.Compile(48000.0);
	REQUIRE(Snapshot);
	REQUIRE(Snapshot->OutputNode != nullptr);
	// The arpeggiator is the patch's lone note sink.
	REQUIRE(Snapshot->NoteSinks.size() == 1);
}
#endif
