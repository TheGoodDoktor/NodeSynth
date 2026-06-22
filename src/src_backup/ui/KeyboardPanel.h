#pragma once

#include <atomic>
#include <cstdint>

#include "graph/AudioCommand.h"

namespace NodeSynth
{
	// Self-contained on-screen keyboard. Owns its own state (octave, velocity,
	// held-note stack) — used to be wired to an FVirtualKeyboard graph node,
	// but note input is a transport concern, not a graph concern, so the panel
	// now stands alone and pushes Note On/Off events into the audio command
	// queue (which the audio thread broadcasts to every voice allocator).
	//
	// Threading: the UI thread mutates the held-note stack and pushes events
	// via the command sink. The atomics expose state to anyone who wants to
	// read it (e.g. a future "current octave" badge somewhere); the audio
	// thread doesn't touch the panel directly.
	class FKeyboardPanel
	{
	public:
		// Renders the keyboard widget at the current ImGui cursor. Sink is
		// the project-level audio command sink (typically FCommandSink with
		// a non-zero ring pointer). Note On / Note Off events are pushed
		// through Sink.NoteOn / Sink.NoteOff.
		void Draw(const FCommandSink& Sink);

		// UI / inspector helpers.
		int32_t GetOctave() const { return Octave.load(std::memory_order_relaxed); }
		float GetVelocity() const { return Velocity.load(std::memory_order_relaxed); }
		bool IsKeyHeld(int32_t SemitoneFromBottomC) const;

	private:
		void PressNote(int32_t Semi, const FCommandSink& Sink);
		void ReleaseNote(int32_t Semi, const FCommandSink& Sink);
		void ReleaseAll(const FCommandSink& Sink);

		static constexpr size_t MaxHeldNotes = 16;
		uint8_t HeldNotes[MaxHeldNotes] = {};
		int8_t HeldSemitones[MaxHeldNotes] = {};
		size_t NumHeldNotes = 0;

		// Cross-thread-readable, but only the UI thread writes.
		std::atomic<int32_t> Octave{ 4 };
		std::atomic<float>   Velocity{ 0.8f };
	};
}
