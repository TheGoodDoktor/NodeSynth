#pragma once

#include <cstdint>

namespace NodeSynth
{
	// Interface for anything that consumes live note events. Implemented by
	// FVoiceAllocator (polyphonic voice management) and FArpeggiator (note
	// reordering / sequencing).
	//
	// Note events are delivered on the audio thread only — by
	// FMidiDeviceManager::Process (MIDI device + on-screen keyboard) and by
	// FAudioGraph::DrainCommands (UI command ring), both at block start before
	// FAudioGraph::Process. Implementations therefore need no locking around
	// their held-note state.
	class INoteSink
	{
	public:
		virtual ~INoteSink() = default;

		// Velocity is normalised 0..1.
		virtual void HandleNoteOn(uint8_t Note, float Velocity) = 0;
		virtual void HandleNoteOff(uint8_t Note) = 0;
	};
}
