#pragma once

#include "dsp/Node.h"

namespace NodeSynth
{
	// Two-input Control adder. Disconnected inputs read as 0 so the node degrades
	// to "pass through whatever is connected."
	class FAdd : public TNodeBase<2, 1>
	{
	public:
		const char* GetTypeName() const override { return "Add"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "A", EPortType::Control },
				{ "B", EPortType::Control },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Control } };
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* A = GetInputBuffer(0);
			const float* B = GetInputBuffer(1);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float Av = (A != nullptr) ? A[I] : 0.0f;
				const float Bv = (B != nullptr) ? B[I] : 0.0f;
				Out[I] = Av + Bv;
			}
		}
	};
}
