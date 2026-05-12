#pragma once

#include <atomic>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Manual gate source. Exists so ADSR / VCA can be wired up and exercised from
	// the UI before MIDI input lands (Phase 2E). Toggle the Held param to fire
	// the envelope.
	class FGateButton : public TNodeBase<0, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Held,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Gate"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return {};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Gate", EPortType::Control,
				"1.0 when Held is checked, otherwise 0.0." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return { { "Held", 0.0f, 1.0f, 0.0f, false, EParamKind::Bool, {},
				"When checked, the gate output is 1.0; otherwise 0.0." } };
		}

		float GetParamValue(uint32_t Index) const override
		{
			return (Index == Param_Held) ? Held.load(std::memory_order_relaxed) : 0.0f;
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			if (Index == Param_Held)
			{
				Held.store(Value, std::memory_order_relaxed);
			}
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float Value = Held.load(std::memory_order_relaxed);
			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				Out[I] = Value;
			}
		}

	private:
		std::atomic<float> Held{ 0.0f };
	};
}
