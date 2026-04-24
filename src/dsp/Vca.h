#pragma once

#include "dsp/Node.h"

namespace NodeSynth
{
	// Audio × Control multiply. Control disconnected => pass-through at unity.
	class FVca : public TNodeBase<2, 1>
	{
	public:
		const char* GetTypeName() const override { return "VCA"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "Audio",   EPortType::Audio },
				{ "Control", EPortType::Control },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio } };
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* Audio = GetInputBuffer(0);
			const float* Control = GetInputBuffer(1);

			if (Audio == nullptr)
			{
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					Out[I] = 0.0f;
				}
				return;
			}

			if (Control == nullptr)
			{
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					Out[I] = Audio[I];
				}
				return;
			}

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				Out[I] = Audio[I] * Control[I];
			}
		}
	};
}
