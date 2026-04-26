#pragma once

#include "dsp/Node.h"

namespace NodeSynth
{
	// Single sink node. Exactly one Output is expected per graph.
	// It has no outputs of its own; the audio callback reads its input buffer
	// directly via GetInputBuffer(0) and copies it to the device.
	class FOutput : public TNodeBase<1, 0>
	{
	public:
		const char* GetTypeName() const override { return "Output"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "In", EPortType::Audio,
				"Master audio that gets routed to the device output." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return {};
		}

		// Singleton sink — the graph compiler already enforces "exactly one
		// FOutput per graph", so cloning per voice is meaningless.
		std::shared_ptr<INode> Clone() const override { return nullptr; }

		void Process(const FProcessContext& Ctx) override
		{
			(void)Ctx;
		}
	};
}
