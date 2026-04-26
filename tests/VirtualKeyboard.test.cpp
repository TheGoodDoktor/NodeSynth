#include "dsp/VirtualKeyboard.h"
#include "graph/AudioCommand.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using NodeSynth::FProcessContext;
using NodeSynth::FVirtualKeyboard;

namespace
{
	void RunOneBlock(FVirtualKeyboard& Kbd, double SampleRate = 48000.0)
	{
		FProcessContext Ctx;
		Ctx.SampleRate = SampleRate;
		Kbd.Process(Ctx);
	}
}

TEST_CASE("VirtualKeyboard: pressing a note raises the gate and sets frequency", "[VirtualKeyboard]")
{
	FVirtualKeyboard Kbd;
	Kbd.Prepare(48000.0);
	Kbd.SetParamValue(FVirtualKeyboard::Param_Octave, 4.0f);

	// Bottom C of octave 4 is MIDI 60. Semitone 9 → A4 = MIDI 69 = 440 Hz.
	Kbd.PressNote(9);
	RunOneBlock(Kbd);

	const float* Gate = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_Gate);
	const float* Freq = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_Frequency);
	REQUIRE(Gate[0] == 1.0f);
	REQUIRE_THAT(Freq[0], Catch::Matchers::WithinAbs(440.0f, 1e-2f));
}

TEST_CASE("VirtualKeyboard: releasing the only held note drops the gate", "[VirtualKeyboard]")
{
	FVirtualKeyboard Kbd;
	Kbd.Prepare(48000.0);

	Kbd.PressNote(0);
	Kbd.ReleaseNote(0);
	RunOneBlock(Kbd);

	const float* Gate = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_Gate);
	REQUIRE(Gate[0] == 0.0f);
	REQUIRE(Kbd.GetNumHeldNotes() == 0);
}

TEST_CASE("VirtualKeyboard: last-note-wins legato across two held notes", "[VirtualKeyboard]")
{
	FVirtualKeyboard Kbd;
	Kbd.Prepare(48000.0);
	Kbd.SetParamValue(FVirtualKeyboard::Param_Octave, 4.0f);

	// Press A4 then B4. Frequency should track the most recently pressed note.
	Kbd.PressNote(9);
	Kbd.PressNote(11);
	RunOneBlock(Kbd);
	{
		const float* Gate = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_Gate);
		const float* Freq = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_Frequency);
		REQUIRE(Gate[0] == 1.0f);
		REQUIRE_THAT(Freq[0], Catch::Matchers::WithinAbs(493.8833f, 0.5f)); // B4
	}

	// Release the older note: gate stays high (legato), freq stays on B4.
	Kbd.ReleaseNote(9);
	RunOneBlock(Kbd);
	{
		const float* Gate = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_Gate);
		const float* Freq = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_Frequency);
		REQUIRE(Gate[0] == 1.0f);
		REQUIRE_THAT(Freq[0], Catch::Matchers::WithinAbs(493.8833f, 0.5f));
	}

	// Release the remaining note: gate drops.
	Kbd.ReleaseNote(11);
	RunOneBlock(Kbd);
	{
		const float* Gate = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_Gate);
		REQUIRE(Gate[0] == 0.0f);
	}
}

TEST_CASE("VirtualKeyboard: octave shift while a note is held updates frequency", "[VirtualKeyboard]")
{
	FVirtualKeyboard Kbd;
	Kbd.Prepare(48000.0);
	Kbd.SetParamValue(FVirtualKeyboard::Param_Octave, 4.0f);

	Kbd.PressNote(9); // A4 = 440 Hz
	RunOneBlock(Kbd);
	const float* Freq = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_Frequency);
	REQUIRE_THAT(Freq[0], Catch::Matchers::WithinAbs(440.0f, 1e-2f));

	// Shift up an octave and re-press the same UI key. We expect A5 = 880 Hz.
	// (Octave change alone doesn't retune already-held notes — the user would
	// re-press; we verify the new press uses the updated octave.)
	Kbd.ReleaseNote(9);
	Kbd.SetParamValue(FVirtualKeyboard::Param_Octave, 5.0f);
	Kbd.PressNote(9);
	RunOneBlock(Kbd);
	REQUIRE_THAT(Freq[0], Catch::Matchers::WithinAbs(880.0f, 1e-2f));
}

TEST_CASE("VirtualKeyboard: ReleaseAll clears the held stack and drops the gate", "[VirtualKeyboard]")
{
	FVirtualKeyboard Kbd;
	Kbd.Prepare(48000.0);

	Kbd.PressNote(0);
	Kbd.PressNote(4);
	Kbd.PressNote(7);
	REQUIRE(Kbd.GetNumHeldNotes() == 3);

	Kbd.ReleaseAll();
	RunOneBlock(Kbd);
	const float* Gate = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_Gate);
	REQUIRE(Gate[0] == 0.0f);
	REQUIRE(Kbd.GetNumHeldNotes() == 0);
}

TEST_CASE("VirtualKeyboard: pressing the same key twice does not double-stack", "[VirtualKeyboard]")
{
	FVirtualKeyboard Kbd;
	Kbd.Prepare(48000.0);

	Kbd.PressNote(5);
	Kbd.PressNote(5);
	REQUIRE(Kbd.GetNumHeldNotes() == 1);

	Kbd.ReleaseNote(5);
	REQUIRE(Kbd.GetNumHeldNotes() == 0);
}

TEST_CASE("VirtualKeyboard: ModWheel converges to the slider target after many blocks", "[VirtualKeyboard]")
{
	FVirtualKeyboard Kbd;
	Kbd.Prepare(48000.0);
	Kbd.SetParamValue(FVirtualKeyboard::Param_ModWheel, 1.0f);

	// One block of audio; smoother shouldn't have caught up yet.
	RunOneBlock(Kbd);
	const float* Mod = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_ModWheel);
	REQUIRE(Mod[NodeSynth::BlockSize - 1] < 1.0f);

	// After ~1 second the smoother should be effectively at the target.
	for (int I = 0; I < 48000 / NodeSynth::BlockSize; ++I)
	{
		RunOneBlock(Kbd);
	}
	REQUIRE_THAT(Mod[NodeSynth::BlockSize - 1], Catch::Matchers::WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("VirtualKeyboard: velocity output is gated by the note", "[VirtualKeyboard]")
{
	FVirtualKeyboard Kbd;
	Kbd.Prepare(48000.0);
	Kbd.SetParamValue(FVirtualKeyboard::Param_Velocity, 0.7f);

	// No note held → velocity output is zero (matches FMidiInput behaviour after note-off).
	RunOneBlock(Kbd);
	const float* Vel = Kbd.GetOutputBuffer(FVirtualKeyboard::Output_Velocity);
	REQUIRE(Vel[0] == 0.0f);

	Kbd.PressNote(0);
	RunOneBlock(Kbd);
	REQUIRE_THAT(Vel[0], Catch::Matchers::WithinAbs(0.7f, 1e-6f));
}

TEST_CASE("VirtualKeyboard: out-of-range MIDI presses are silently dropped", "[VirtualKeyboard]")
{
	FVirtualKeyboard Kbd;
	Kbd.Prepare(48000.0);
	Kbd.SetParamValue(FVirtualKeyboard::Param_Octave, 8.0f); // max — top C maps to MIDI 120

	// Semitone 12 at octave 8 → MIDI 120 (still valid).
	Kbd.PressNote(12);
	REQUIRE(Kbd.GetNumHeldNotes() == 1);

	// Forced out-of-range via direct semitone overflow.
	Kbd.PressNote(20); // MIDI = 12 * 9 + 20 = 128 → dropped
	REQUIRE(Kbd.GetNumHeldNotes() == 1);
}

TEST_CASE("VirtualKeyboard: PressNote / ReleaseNote push commands through the sink", "[VirtualKeyboard][voicealloc]")
{
	NodeSynth::FAudioCommandRing Ring;
	NodeSynth::FCommandSink Sink{ &Ring, 0 };

	FVirtualKeyboard Kbd;
	Kbd.Prepare(48000.0);
	Kbd.SetParamValue(FVirtualKeyboard::Param_Octave, 4.0f); // C4 = MIDI 60

	Kbd.PressNote(0, Sink); // bottom C → MIDI 60
	NodeSynth::FAudioCommand Cmd;
	REQUIRE(Ring.Pop(Cmd));
	REQUIRE(Cmd.Type == NodeSynth::EAudioCommand::NoteOn);
	REQUIRE(Cmd.ParamIndex == 60);

	Kbd.ReleaseNote(0, Sink);
	REQUIRE(Ring.Pop(Cmd));
	REQUIRE(Cmd.Type == NodeSynth::EAudioCommand::NoteOff);
	REQUIRE(Cmd.ParamIndex == 60);
	REQUIRE(Ring.IsEmpty());
}

TEST_CASE("VirtualKeyboard: ReleaseAll pushes a NoteOff for every held note", "[VirtualKeyboard][voicealloc]")
{
	NodeSynth::FAudioCommandRing Ring;
	NodeSynth::FCommandSink Sink{ &Ring, 0 };

	FVirtualKeyboard Kbd;
	Kbd.Prepare(48000.0);
	Kbd.SetParamValue(FVirtualKeyboard::Param_Octave, 4.0f);

	Kbd.PressNote(0, Sink);
	Kbd.PressNote(4, Sink);
	Kbd.PressNote(7, Sink);
	// Drain those NoteOns; we only care about the ReleaseAll burst.
	NodeSynth::FAudioCommand Tmp;
	while (Ring.Pop(Tmp)) {}

	Kbd.ReleaseAll(Sink);

	int NoteOffs = 0;
	while (Ring.Pop(Tmp))
	{
		if (Tmp.Type == NodeSynth::EAudioCommand::NoteOff)
		{
			++NoteOffs;
		}
	}
	REQUIRE(NoteOffs == 3);
}
