#include "dsp/MidiInput.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/VoiceAllocator.h"
#include "graph/AudioCommand.h"
#include "graph/Graph.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using NodeSynth::FAudioCommand;
using NodeSynth::FAudioCommandRing;
using NodeSynth::FAudioGraph;
using NodeSynth::FCommandSink;
using NodeSynth::FGraphModel;
using NodeSynth::FMidiInput;
using NodeSynth::FNodeId;
using NodeSynth::FOutput;
using NodeSynth::FProcessContext;
using NodeSynth::FVoiceAllocator;

namespace
{
	std::shared_ptr<FAudioGraph> CompileWith(FGraphModel& Model)
	{
		return Model.Compile(48000.0);
	}
}

TEST_CASE("VoiceAllocator: 8 simultaneous NoteOns occupy 8 voices", "[voicealloc]")
{
	FVoiceAllocator A;
	A.Prepare(48000.0);

	for (uint8_t I = 0; I < 8; ++I)
	{
		A.HandleNoteOn(static_cast<uint8_t>(60 + I), 1.0f);
	}

	for (size_t I = 0; I < 8; ++I)
	{
		const auto& V = A.GetVoice(I);
		REQUIRE(V.bGate == true);
		REQUIRE(V.Note == 60 + I);
		REQUIRE_THAT(V.Velocity, Catch::Matchers::WithinAbs(1.0f, 1e-6f));
	}
}

TEST_CASE("VoiceAllocator: 9th note steals the oldest still-held voice", "[voicealloc][stealing]")
{
	FVoiceAllocator A;
	A.Prepare(48000.0);
	NodeSynth::FProcessContext Ctx;

	// Press 8 notes with Process between each so AgeSamples differs.
	for (uint8_t I = 0; I < 8; ++I)
	{
		A.HandleNoteOn(static_cast<uint8_t>(60 + I), 1.0f);
		A.Process(Ctx); // advance time so this voice is now older than the next
	}

	// All 8 held; voice 0 should be oldest (got first NoteOn).
	REQUIRE(A.GetVoice(0).bGate);
	REQUIRE(A.GetVoice(0).Note == 60);

	// 9th note steals the oldest held voice — that's voice 0.
	A.HandleNoteOn(72, 1.0f);
	REQUIRE(A.GetVoice(0).bGate);
	REQUIRE(A.GetVoice(0).Note == 72);

	// The other 7 should still be holding their original notes.
	for (uint8_t I = 1; I < 8; ++I)
	{
		REQUIRE(A.GetVoice(I).bGate);
		REQUIRE(A.GetVoice(I).Note == 60 + I);
	}
}

TEST_CASE("VoiceAllocator: a released voice gets a new note before any held voice is stolen", "[voicealloc][stealing]")
{
	FVoiceAllocator A;
	A.Prepare(48000.0);
	NodeSynth::FProcessContext Ctx;

	// Fill all 8 voices.
	for (uint8_t I = 0; I < 8; ++I)
	{
		A.HandleNoteOn(static_cast<uint8_t>(60 + I), 1.0f);
		A.Process(Ctx);
	}
	// Release voice 4 (note 64). It's now in the release tail.
	A.HandleNoteOff(64);
	A.Process(Ctx);

	// 9th note: should land in voice 4, not steal voice 0.
	A.HandleNoteOn(80, 1.0f);
	REQUIRE(A.GetVoice(4).bGate);
	REQUIRE(A.GetVoice(4).Note == 80);

	// Voice 0 must still hold its original note.
	REQUIRE(A.GetVoice(0).bGate);
	REQUIRE(A.GetVoice(0).Note == 60);
}

TEST_CASE("VoiceAllocator: prefers fully-released over still-tailing", "[voicealloc][stealing]")
{
	FVoiceAllocator A;
	A.Prepare(48000.0);
	NodeSynth::FProcessContext Ctx;

	// Fill all 8 voices.
	for (uint8_t I = 0; I < 8; ++I)
	{
		A.HandleNoteOn(static_cast<uint8_t>(60 + I), 1.0f);
		A.Process(Ctx);
	}

	// Release voice 2 (oldest of two we'll release).
	A.HandleNoteOff(62);

	// Advance well past ReleaseThresholdSamples (100 ms = 4800 samples = 75 blocks).
	for (int B = 0; B < 100; ++B) { A.Process(Ctx); }

	// Now release voice 6. Voice 6 is in its release tail; voice 2 is fully
	// released.
	A.HandleNoteOff(66);
	A.Process(Ctx);

	// 9th note: should pick voice 2 (fully released) over voice 6 (tailing),
	// even though voice 6 has been released more recently — fully-released
	// always wins over tailing.
	A.HandleNoteOn(90, 1.0f);
	REQUIRE(A.GetVoice(2).bGate);
	REQUIRE(A.GetVoice(2).Note == 90);
	REQUIRE_FALSE(A.GetVoice(6).bGate);
}

TEST_CASE("VoiceAllocator: NoteOff clears the matching voice's gate", "[voicealloc]")
{
	FVoiceAllocator A;
	A.Prepare(48000.0);

	A.HandleNoteOn(60, 0.8f);
	A.HandleNoteOn(64, 0.8f);
	A.HandleNoteOn(67, 0.8f);
	REQUIRE(A.GetVoice(0).bGate);
	REQUIRE(A.GetVoice(1).bGate);
	REQUIRE(A.GetVoice(2).bGate);

	A.HandleNoteOff(64);
	// Voice that held 64 (the second allocation) should drop its gate.
	REQUIRE(A.GetVoice(0).bGate);
	REQUIRE_FALSE(A.GetVoice(1).bGate);
	REQUIRE(A.GetVoice(2).bGate);
}

TEST_CASE("VoiceAllocator: same-note retrigger reuses the same voice", "[voicealloc]")
{
	FVoiceAllocator A;
	A.Prepare(48000.0);

	A.HandleNoteOn(60, 0.5f);
	A.HandleNoteOn(60, 0.9f); // retrigger
	// Voice 0 holds note 60 with the new velocity; voice 1 should still be free.
	REQUIRE(A.GetVoice(0).bGate);
	REQUIRE(A.GetVoice(0).Note == 60);
	REQUIRE_THAT(A.GetVoice(0).Velocity, Catch::Matchers::WithinAbs(0.9f, 1e-6f));
	REQUIRE_FALSE(A.GetVoice(1).bGate);
}

TEST_CASE("VoiceAllocator: NumVoices param caps allocation", "[voicealloc]")
{
	FVoiceAllocator A;
	A.Prepare(48000.0);

	// NumVoices choice indices: 0 = "1", 1 = "2", 2 = "4", 3 = "8".
	A.SetParamValue(FVoiceAllocator::Param_NumVoices, 1.0f); // -> 2 voices
	REQUIRE(A.ActiveVoiceCount() == 2);

	A.HandleNoteOn(60, 1.0f);
	A.HandleNoteOn(64, 1.0f);
	A.HandleNoteOn(67, 1.0f); // dropped — only 2 voices active

	REQUIRE(A.GetVoice(0).bGate);
	REQUIRE(A.GetVoice(1).bGate);
	REQUIRE_FALSE(A.GetVoice(2).bGate);
}

TEST_CASE("VoiceAllocator: Process emits voice 0 state on standard outputs (3E-3 stub)", "[voicealloc]")
{
	FVoiceAllocator A;
	A.Prepare(48000.0);
	A.HandleNoteOn(69, 0.7f); // A4 → 440 Hz

	FProcessContext Ctx;
	A.Process(Ctx);

	const float* Gate = A.GetOutputBuffer(FVoiceAllocator::Output_Gate);
	const float* Freq = A.GetOutputBuffer(FVoiceAllocator::Output_Frequency);
	const float* Vel = A.GetOutputBuffer(FVoiceAllocator::Output_Velocity);
	const float* Note = A.GetOutputBuffer(FVoiceAllocator::Output_Note);

	REQUIRE(Gate[0] == 1.0f);
	REQUIRE_THAT(Freq[0], Catch::Matchers::WithinAbs(440.0f, 1e-3f));
	REQUIRE_THAT(Vel[0], Catch::Matchers::WithinAbs(0.7f, 1e-6f));
	REQUIRE_THAT(Note[0], Catch::Matchers::WithinAbs(69.0f, 1e-6f));
}

TEST_CASE("VoiceAllocator: Clone() returns nullptr (non-cloneable source)", "[voicealloc][clone]")
{
	FVoiceAllocator A;
	REQUIRE(A.Clone() == nullptr);
}

TEST_CASE("VoiceAllocator: graph compile populates Allocators and routes NoteOn", "[voicealloc][graph]")
{
	// Allocator (poly Control) → Oscillator → Output. The Oscillator must be
	// marked per-voice for the Control link to validate (poly → mono Control is
	// rejected by the partition step). Audio path Osc → Out is per-voice → mono
	// audio, which the compiler handles via a synthesised voice mixer.
	FGraphModel Model;
	auto Alloc = std::make_shared<NodeSynth::FVoiceAllocator>();
	auto Osc = std::make_shared<NodeSynth::FOscillator>();
	auto Out = std::make_shared<FOutput>();
	const FNodeId AllocId = Model.AddNode(Alloc);
	const FNodeId OscId = Model.AddNode(Osc);
	const FNodeId OutId = Model.AddNode(Out);
	REQUIRE(Model.SetNodePerVoice(OscId, true));
	REQUIRE(Model.AddLink(AllocId, FVoiceAllocator::Output_Frequency,
		OscId, NodeSynth::FOscillator::Input_Frequency) != 0);
	REQUIRE(Model.AddLink(OscId, 0, OutId, 0) != 0);

	auto Snapshot = CompileWith(Model);
	REQUIRE(Snapshot->Allocators.size() == 1);
	REQUIRE(Snapshot->Allocators[0] == Alloc.get());

	FAudioCommandRing Ring;
	Ring.Push(FAudioCommand::MakeNoteOn(60, 1.0f));
	Ring.Push(FAudioCommand::MakeNoteOn(64, 1.0f));
	Snapshot->DrainCommands(Ring);

	REQUIRE(Alloc->GetVoice(0).bGate);
	REQUIRE(Alloc->GetVoice(0).Note == 60);
	REQUIRE(Alloc->GetVoice(1).bGate);
	REQUIRE(Alloc->GetVoice(1).Note == 64);
}

TEST_CASE("VoiceAllocator: DrainCommands broadcasts NoteOn / NoteOff to every allocator in snapshot", "[voicealloc][graph]")
{
	// Sidestep reachability + Output-singleton constraints: build a snapshot
	// directly with two allocators in its Allocators list, push events through
	// the ring, drain. Verifies the broadcast loop in DrainCommands itself.
	auto AllocA = std::make_shared<FVoiceAllocator>();
	auto AllocB = std::make_shared<FVoiceAllocator>();
	AllocA->Prepare(48000.0);
	AllocB->Prepare(48000.0);

	auto Snapshot = std::make_shared<FAudioGraph>();
	Snapshot->OrderedNodes.push_back(AllocA);
	Snapshot->OrderedNodes.push_back(AllocB);
	Snapshot->Allocators.push_back(AllocA.get());
	Snapshot->Allocators.push_back(AllocB.get());

	FAudioCommandRing Ring;
	Ring.Push(FAudioCommand::MakeNoteOn(72, 1.0f));
	Ring.Push(FAudioCommand::MakeNoteOn(76, 1.0f));
	Snapshot->DrainCommands(Ring);

	// Both allocators should have voices for both notes.
	REQUIRE(AllocA->GetVoice(0).Note == 72);
	REQUIRE(AllocA->GetVoice(0).bGate);
	REQUIRE(AllocA->GetVoice(1).Note == 76);
	REQUIRE(AllocB->GetVoice(0).Note == 72);
	REQUIRE(AllocB->GetVoice(0).bGate);
	REQUIRE(AllocB->GetVoice(1).Note == 76);

	Ring.Push(FAudioCommand::MakeNoteOff(72));
	Snapshot->DrainCommands(Ring);
	REQUIRE_FALSE(AllocA->GetVoice(0).bGate);
	REQUIRE_FALSE(AllocB->GetVoice(0).bGate);
}

TEST_CASE("VoiceAllocator: only reachable allocators land in the snapshot", "[voicealloc][graph]")
{
	FGraphModel Model;
	auto Reachable = std::make_shared<FVoiceAllocator>();
	auto Unreachable = std::make_shared<FVoiceAllocator>();
	auto Osc = std::make_shared<NodeSynth::FOscillator>();
	auto Out = std::make_shared<FOutput>();
	const FNodeId ReachId = Model.AddNode(Reachable);
	const FNodeId UnreachId = Model.AddNode(Unreachable);
	const FNodeId OscId = Model.AddNode(Osc);
	const FNodeId OutId = Model.AddNode(Out);
	(void)UnreachId;
	REQUIRE(Model.SetNodePerVoice(OscId, true));
	REQUIRE(Model.AddLink(ReachId, FVoiceAllocator::Output_Frequency,
		OscId, NodeSynth::FOscillator::Input_Frequency) != 0);
	REQUIRE(Model.AddLink(OscId, 0, OutId, 0) != 0);

	auto Snapshot = CompileWith(Model);
	REQUIRE(Snapshot->Allocators.size() == 1);
	REQUIRE(Snapshot->Allocators[0] == Reachable.get());
}

TEST_CASE("VoiceAllocator: Glide=0 produces instant pitch jumps on note change", "[voicealloc][glide]")
{
	FVoiceAllocator A;
	A.SetParamValue(FVoiceAllocator::Param_NumVoices, 0.0f);  // 1 voice
	A.SetParamValue(FVoiceAllocator::Param_Glide, 0.0f);
	A.Prepare(48000.0);

	// First NoteOn: voice 0 takes A4 (440 Hz).
	A.HandleNoteOn(69, 1.0f);
	NodeSynth::FProcessContext Ctx;
	A.Process(Ctx);
	const float* FreqBuf = A.GetVoiceOutputBuffer(FVoiceAllocator::Output_Frequency, 0);
	REQUIRE_THAT(FreqBuf[NodeSynth::BlockSize - 1],
		Catch::Matchers::WithinAbs(440.0f, 1e-2f));

	// NoteOff + NoteOn for a different note (E5 = MIDI 76) on the same voice.
	A.HandleNoteOff(69);
	A.HandleNoteOn(76, 1.0f);
	A.Process(Ctx);
	// Glide=0 → first sample of the next block already at the new pitch.
	const float Expected = 440.0f * std::pow(2.0f, (76.0f - 69.0f) / 12.0f);
	REQUIRE_THAT(FreqBuf[0], Catch::Matchers::WithinAbs(Expected, 1e-1f));
}

TEST_CASE("VoiceAllocator: Glide>0 slides frequency between consecutive notes", "[voicealloc][glide]")
{
	FVoiceAllocator A;
	// Single voice so successive NoteOns reliably reuse voice 0 (otherwise
	// the stealing policy may pick a fresh voice and the smoother under
	// test never sees the new note).
	A.SetParamValue(FVoiceAllocator::Param_NumVoices, 0.0f);  // 1 voice
	// 30 ms time constant — short enough that 300 blocks (≈ 13 τ) settles
	// the smoother for the test's stability checks.
	A.SetParamValue(FVoiceAllocator::Param_Glide, 30.0f);
	A.Prepare(48000.0);

	A.HandleNoteOn(60, 1.0f);
	NodeSynth::FProcessContext Ctx;
	for (int B = 0; B < 300; ++B) { A.Process(Ctx); }
	const float* FreqBuf = A.GetVoiceOutputBuffer(FVoiceAllocator::Output_Frequency, 0);
	const float StableFirstNote = FreqBuf[NodeSynth::BlockSize - 1];

	// New note: target a full octave up.
	A.HandleNoteOff(60);
	A.HandleNoteOn(72, 1.0f);
	const float NewTarget = StableFirstNote * 2.0f;

	// A few blocks into the glide: frequency must have moved past the old
	// note but not yet reached the new target.
	for (int B = 0; B < 5; ++B) { A.Process(Ctx); }
	REQUIRE(FreqBuf[NodeSynth::BlockSize - 1] > StableFirstNote + 1.0f);
	REQUIRE(FreqBuf[NodeSynth::BlockSize - 1] < NewTarget - 1.0f);

	// After settling, smoother lands on the new target.
	for (int B = 0; B < 500; ++B) { A.Process(Ctx); }
	REQUIRE_THAT(FreqBuf[NodeSynth::BlockSize - 1],
		Catch::Matchers::WithinAbs(NewTarget, 0.5f));
}

TEST_CASE("VoiceAllocator: same-note retrigger keeps frequency steady", "[voicealloc][glide]")
{
	FVoiceAllocator A;
	A.SetParamValue(FVoiceAllocator::Param_NumVoices, 0.0f);  // 1 voice
	A.SetParamValue(FVoiceAllocator::Param_Glide, 30.0f);
	A.Prepare(48000.0);

	A.HandleNoteOn(60, 1.0f);
	NodeSynth::FProcessContext Ctx;
	for (int B = 0; B < 300; ++B) { A.Process(Ctx); }
	const float* FreqBuf = A.GetVoiceOutputBuffer(FVoiceAllocator::Output_Frequency, 0);
	const float StableFreq = FreqBuf[NodeSynth::BlockSize - 1];

	// Retrigger the same note; smoother target stays put → frequency stays put.
	A.HandleNoteOn(60, 1.0f);
	A.Process(Ctx);
	REQUIRE_THAT(FreqBuf[NodeSynth::BlockSize - 1],
		Catch::Matchers::WithinAbs(StableFreq, 0.1f));
}

TEST_CASE("VoiceAllocator: NoteOff for a never-pressed note is a no-op", "[voicealloc]")
{
	FVoiceAllocator A;
	A.Prepare(48000.0);
	A.HandleNoteOn(60, 0.8f);
	A.HandleNoteOff(72); // never pressed
	REQUIRE(A.GetVoice(0).bGate);
	REQUIRE(A.GetVoice(0).Note == 60);
}
