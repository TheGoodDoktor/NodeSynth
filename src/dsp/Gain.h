#pragma once

#include <atomic>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	class FGain : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Gain,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Gain"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "In", EPortType::Audio } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return { { "Gain", 0.0f, 2.0f, 1.0f, false, EParamKind::Float, {} } };
		}

		float GetParamValue(uint32_t Index) const override
		{
			return (Index == Param_Gain) ? Gain.load(std::memory_order_relaxed) : 0.0f;
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			if (Index == Param_Gain)
			{
				Gain.store(Value, std::memory_order_relaxed);
			}
		}

		void Prepare(double SampleRate) override
		{
			GainSmoother.Prepare(SampleRate);
			GainSmoother.Reset(Gain.load(std::memory_order_relaxed));
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* In = GetInputBuffer(0);
			GainSmoother.SetTarget(Gain.load(std::memory_order_relaxed));

			if (In == nullptr)
			{
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					Out[I] = 0.0f;
				}
			}
			else
			{
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					Out[I] = In[I] * GainSmoother.Tick();
				}
			}
		}

	private:
		std::atomic<float> Gain{ 1.0f };
		FOnePoleSmoother GainSmoother;
	};
}
