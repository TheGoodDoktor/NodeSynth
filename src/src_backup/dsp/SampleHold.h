#pragma once

#include "dsp/Node.h"

namespace NodeSynth
{
	// Sample-and-hold. On every rising edge of Trigger (low → high, threshold 0.5),
	// captures the current In value and holds it on Out until the next rising edge.
	// Disconnected inputs read as 0.
	class FSampleHold : public TNodeBase<2, 1>
	{
	public:
		enum EInput : uint32_t
		{
			Input_In,
			Input_Trigger,
		};

		const char* GetTypeName() const override { return "SampleHold"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "In",      EPortType::Control,
					"Signal to sample. Disconnected = 0." },
				{ "Trigger", EPortType::Control,
					"Rising edge (threshold 0.5) latches In into Out." },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Control,
				"Last latched In value, held until the next trigger." } };
		}

		void Prepare(double /*SampleRate*/) override
		{
			HeldValue = 0.0f;
			bPrevTrigHigh = false;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* In = GetInputBuffer(Input_In);
			const float* Trig = GetInputBuffer(Input_Trigger);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float TrigV = (Trig != nullptr) ? Trig[I] : 0.0f;
				const bool bHigh = TrigV > 0.5f;
				if (bHigh && !bPrevTrigHigh)
				{
					HeldValue = (In != nullptr) ? In[I] : 0.0f;
				}
				bPrevTrigHigh = bHigh;
				Out[I] = HeldValue;
			}
		}

	private:
		float HeldValue = 0.0f;
		bool bPrevTrigHigh = false;
	};
}
