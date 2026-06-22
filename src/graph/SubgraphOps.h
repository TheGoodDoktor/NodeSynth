#pragma once

#include <vector>

#include "dsp/Node.h"

namespace NodeSynth
{
	class FGraphModel;

	// Wraps the given selected nodes of Patch into a new subgraph instance,
	// auto-promoting links that cross the selection boundary to pins (incoming
	// → input pins; outgoing → output pins, deduped by source port). Nodes that
	// can't be enclosed (the Output sink, the voice allocator, subgraph boundary
	// nodes) or can't be cloned are dropped from the selection. The new
	// definition is registered in Patch's subgraph map under a unique name and
	// the selected nodes are replaced by a single instance at their centroid.
	//
	// Returns the new instance node id, or 0 if nothing was grouped. Pure graph
	// surgery — does not touch edit history or any editor/canvas state (the
	// caller decides whether to record history).
	FNodeId GroupNodesIntoSubgraph(FGraphModel& Patch, const std::vector<FNodeId>& Selected);
}
