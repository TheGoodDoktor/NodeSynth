#pragma once

namespace NodeSynth
{
	class FMicInput;

	// Device picker + rescan + feedback warning for a Mic Input node. Opening a
	// capture device is a UI-thread action, so the device combo lives here
	// rather than riding the generic Choice-param widget (which would push a
	// SetParam command onto the audio thread).
	void DrawMicInputUI(FMicInput& Mic);
}
