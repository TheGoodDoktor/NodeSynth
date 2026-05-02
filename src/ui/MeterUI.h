#pragma once

namespace NodeSynth
{
	class FMeter;

	// Draws Peak (with hold) and RMS bars + numeric dB readout for FMeter.
	// Used in the property panel — wide bars + range annotation.
	void DrawMeterUI(FMeter& Meter);

	// Compact in-node version: two narrow horizontal bars + dB labels,
	// rendered between the meter's input/output ports and ed::EndNode().
	// Drawn directly into the imgui-node-editor's node body so the user
	// can see the live levels without selecting the node.
	void DrawMeterNodeBody(FMeter& Meter);
}
