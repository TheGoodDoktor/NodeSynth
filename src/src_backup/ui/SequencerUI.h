#pragma once

#include "graph/AudioCommand.h"

namespace NodeSynth
{
	class FSequencer;

	// Renders the 16-step grid (enable, pitch, velocity, gate-length per step)
	// in the property panel. Param edits go through FCommandSink so they
	// flow through the audio command queue alongside other SetParam writes.
	void DrawSequencerUI(FSequencer& Seq, const FCommandSink& Sink);
}
