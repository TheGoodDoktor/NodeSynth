#pragma once

namespace NodeSynth
{
	class FAdsr;

	// Draws an ADSR envelope curve in the property panel based on the node's
	// current Attack/Decay/Sustain/Release params. Read-only — interaction is
	// still via the standard sliders rendered above it.
	void DrawAdsrUI(FAdsr& Adsr);
}
