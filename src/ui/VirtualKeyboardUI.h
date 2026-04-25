#pragma once

namespace NodeSynth
{
	class FVirtualKeyboard;

	// Draws the property-panel UI for FVirtualKeyboard: octave shift, mod slider,
	// piano-key row with mouse + computer-keyboard interaction. Lives in the UI
	// layer so it can pull in <imgui.h> without coupling the DSP layer to it.
	void DrawVirtualKeyboardUI(FVirtualKeyboard& Kbd);
}
