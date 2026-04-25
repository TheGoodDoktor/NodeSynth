#pragma once

#include "graph/AudioCommand.h"

namespace NodeSynth
{
	class FVirtualKeyboard;

	// Draws the property-panel UI for FVirtualKeyboard: octave shift, mod slider,
	// piano-key row with mouse + computer-keyboard interaction. Lives in the UI
	// layer so it can pull in <imgui.h> without coupling the DSP layer to it.
	//
	// Sink is used to publish param changes (Octave / Mod) onto the audio command
	// queue so they stay ordered with other SetParam edits. Note presses are NOT
	// routed through the queue yet — they still mutate the keyboard's atomics
	// directly (event-style commands land alongside polyphony).
	void DrawVirtualKeyboardUI(FVirtualKeyboard& Kbd, const FCommandSink& Sink);
}
