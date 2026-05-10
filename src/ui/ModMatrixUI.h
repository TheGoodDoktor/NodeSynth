#pragma once

#include <cstdint>

namespace NodeSynth
{
	class FModulationMatrix;
	class FGraphModel;

	// Property-panel UI for FModulationMatrix: 8×8 bipolar slider grid +
	// per-output offset row. Right-click any cell for Zero / Invert.
	// Cells whose Src column has no incoming link get a dim tint to
	// signal "this depth currently does nothing". Slider drags push
	// SetParam edits through CommandRing; one history entry per drag via
	// the same activation/deactivation pattern the standard widget loop
	// uses.
	void DrawModMatrixUI(
		FModulationMatrix& Matrix,
		uint64_t NodeId,
		const FGraphModel& Model,
		const struct FCommandSink& Sink,
		class FEditHistory* History);
}
