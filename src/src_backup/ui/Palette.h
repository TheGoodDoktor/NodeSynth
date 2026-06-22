#pragma once

namespace NodeSynth
{
	// Renders a list of all registered node types as drag sources. Drop them onto
	// the graph editor (the editor registers itself as the matching drop target).
	// Payload type id used between source and target.
	inline constexpr const char* PaletteDragPayloadId = "NODESYNTH_NODE_TYPE";

	void DrawNodePalette();
}
