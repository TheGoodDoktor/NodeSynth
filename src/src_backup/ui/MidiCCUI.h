#pragma once

#include <cstdint>

namespace NodeSynth
{
	class FMidiCC;

	// Property-panel UI for FMidiCC: standard params render via the
	// generic widget loop, then this hook adds a Learn button and a
	// "last raw value" indicator. The Learn flow piggybacks on the
	// existing FGraphEditorPanel::LearnTarget* fields — the drain code
	// in Editor.cpp distinguishes FMidiCC-learn from per-param-learn
	// by checking the target node's type.
	//
	// Returns true if the user clicked Learn, with the node's id in
	// OutNodeId so the editor panel can stash it as the learn target.
	bool DrawMidiCCUI(FMidiCC& Cc, uint64_t NodeId, uint64_t& OutLearnNodeId);
}
