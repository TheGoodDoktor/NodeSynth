#pragma once

#include <memory>
#include <vector>

#include "dsp/Node.h"

namespace NodeSynth
{
	// One entry per concrete node type. TypeName matches INode::GetTypeName() so
	// it can drive the icon lookup; MenuLabel is the human-readable string shown
	// in the create-node menu and the node palette; Description is the tooltip
	// shown on hover in the palette; Make builds a fresh instance.
	struct FNodeRegistration
	{
		const char* TypeName;
		const char* MenuLabel;
		const char* Description;
		std::shared_ptr<INode> (*Make)();
	};

	const std::vector<FNodeRegistration>& GetNodeRegistry();
}
