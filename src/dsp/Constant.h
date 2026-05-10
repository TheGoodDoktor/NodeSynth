#pragma once

#include <atomic>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Constant Control source. Outputs the Value param continuously. Useful for
	// offsets, fixed bias points, and as a slider you can wire into any Control
	// input.
	class FConstant : public TNodeBase<0, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Value,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Constant"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return {};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Control,
				"Constant signal at the Value param." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			FParamInfo P{};
			P.Name = "Value";
			P.MinValue = -20000.0f;
			P.MaxValue = 20000.0f;
			P.DefaultValue = 0.0f;
			P.bLogarithmic = false;
			P.Kind = EParamKind::Float;
			P.Description =
				"Constant value emitted continuously on the output.\n"
				"Drag to scrub; double-click or Ctrl+click to type.";
			P.bUseInputBox = true;
			return { P };
		}

		float GetParamValue(uint32_t Index) const override
		{
			return (Index == Param_Value) ? Value.load(std::memory_order_relaxed) : 0.0f;
		}

		void SetParamValue(uint32_t Index, float InValue) override
		{
			if (Index == Param_Value)
			{
				Value.store(InValue, std::memory_order_relaxed);
			}
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float V = Value.load(std::memory_order_relaxed);
			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				Out[I] = V;
			}
		}

	private:
		std::atomic<float> Value{ 0.0f };
	};
}
