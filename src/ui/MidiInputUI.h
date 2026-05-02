#pragma once

namespace NodeSynth
{
	class FMidiInput;

	// Status block under the standard FMidiInput params: lists detected
	// devices, shows which port is open, surfaces the "no devices found"
	// case prominently so the user knows whether RtMidi is seeing their
	// hardware at all.
	void DrawMidiInputUI(FMidiInput& Node);
}
