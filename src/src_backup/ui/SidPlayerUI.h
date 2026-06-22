#pragma once

namespace NodeSynth
{
	class FSidPlayer;

	// Draws a status block under the standard FSidPlayer params: load status,
	// tune name / author / released, subtune count, region/model. Shown as
	// red text on load failure with the error code.
	void DrawSidPlayerUI(FSidPlayer& Player);
}
