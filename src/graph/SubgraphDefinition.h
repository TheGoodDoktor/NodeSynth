#pragma once

#include <string>
#include <vector>

#include "dsp/Node.h"
#include "graph/Graph.h"

namespace NodeSynth
{
	// One declared boundary pin of a subgraph. Order within the owning
	// definition's InputPins / OutputPins vector matches the port index on
	// both the FSubgraphInputs / FSubgraphOutputs boundary nodes (inside the
	// subgraph) and the FSubgraph instance node (in the parent graph).
	struct FSubgraphPin
	{
		std::string Name;                       // Display label on the port.
		EPortType Type = EPortType::Audio;      // Audio or Control.
		std::string Description;                // Optional tooltip. Empty disables.
	};

	// A reusable subgraph: a declared signature (input / output pins) plus an
	// internal graph that realises it. Compiled by macro-expansion at
	// FGraphModel::Compile time, so it carries no runtime state of its own.
	//
	// Stored standalone as a `.nspg` asset and embedded by name in a patch's
	// top-level "subgraphs" block. See docs/PLAN-SUBGRAPHS.md.
	struct FSubgraphDefinition
	{
		std::string Name;
		std::vector<FSubgraphPin> InputPins;
		std::vector<FSubgraphPin> OutputPins;
		// The internal graph. Owns its own nodes / links / metadata. Does NOT
		// contain an FOutput — the boundary nodes (FSubgraphInputs /
		// FSubgraphOutputs, added in SG.2) stand in for the patch sink.
		FGraphModel InternalGraph;
	};
}
