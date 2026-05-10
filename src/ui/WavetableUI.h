#pragma once

namespace NodeSynth
{
	class FWavetableOscillator;

	// Property-panel UI for FWavetableOscillator: library dropdown for
	// picking a bundled or user wavetable, "..." button for arbitrary
	// .wav paths, and a frame preview rendered from the current Position.
	void DrawWavetableUI(FWavetableOscillator& Wt);
}
