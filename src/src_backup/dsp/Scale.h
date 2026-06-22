#pragma once

#include <atomic>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Linear range remapper for Control signals. Maps [InMin, InMax] → [OutMin, OutMax].
	// Defaults remap a bipolar LFO ([-1, 1]) to unipolar ([0, 1]).
	// Values outside InMin..InMax are extrapolated, not clamped — chain a Min/Max
	// node later if clamping is wanted.
	class FScale : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_InMin,
			Param_InMax,
			Param_OutMin,
			Param_OutMax,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Scale"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "In", EPortType::Control,
				"Source signal. Values outside [InMin, InMax] are extrapolated, not clamped." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Control,
				"Linearly remapped to [OutMin, OutMax]." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			auto MakeEndpoint = [](const char* Name, float Default, const char* Desc)
			{
				FParamInfo P{};
				P.Name = Name;
				P.MinValue = -20000.0f;
				P.MaxValue = 20000.0f;
				P.DefaultValue = Default;
				P.Kind = EParamKind::Float;
				P.Description = Desc;
				P.bUseInputBox = true;
				return P;
			};
			return
			{
				MakeEndpoint("InMin", -1.0f,
					"Lower bound of the input range.\n"
					"Drag to scrub; double-click or Ctrl+click to type."),
				MakeEndpoint("InMax",  1.0f, "Upper bound of the input range."),
				MakeEndpoint("OutMin", 0.0f, "Lower bound of the output range."),
				MakeEndpoint("OutMax", 1.0f, "Upper bound of the output range."),
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_InMin:  return InMin.load(std::memory_order_relaxed);
				case Param_InMax:  return InMax.load(std::memory_order_relaxed);
				case Param_OutMin: return OutMin.load(std::memory_order_relaxed);
				case Param_OutMax: return OutMax.load(std::memory_order_relaxed);
				default:           return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_InMin:  InMin.store(Value, std::memory_order_relaxed); break;
				case Param_InMax:  InMax.store(Value, std::memory_order_relaxed); break;
				case Param_OutMin: OutMin.store(Value, std::memory_order_relaxed); break;
				case Param_OutMax: OutMax.store(Value, std::memory_order_relaxed); break;
				default: break;
			}
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* In = GetInputBuffer(0);
			const float Imin = InMin.load(std::memory_order_relaxed);
			const float Imax = InMax.load(std::memory_order_relaxed);
			const float Omin = OutMin.load(std::memory_order_relaxed);
			const float Omax = OutMax.load(std::memory_order_relaxed);

			// Degenerate input range: emit OutMin (avoids divide-by-zero, well-defined).
			const float Span = Imax - Imin;
			if (Span == 0.0f)
			{
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					Out[I] = Omin;
				}
				return;
			}
			const float InvSpan = 1.0f / Span;
			const float OutSpan = Omax - Omin;

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float V = (In != nullptr) ? In[I] : 0.0f;
				Out[I] = Omin + (V - Imin) * InvSpan * OutSpan;
			}
		}

	private:
		std::atomic<float> InMin{ -1.0f };
		std::atomic<float> InMax{ 1.0f };
		std::atomic<float> OutMin{ 0.0f };
		std::atomic<float> OutMax{ 1.0f };
	};
}
