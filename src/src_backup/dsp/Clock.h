#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"

namespace NodeSynth
{
	// 50%-duty-cycle gate clock at a configurable BPM. Drives a sequencer's
	// Clock input, or any node that wants a regular trigger.
	//
	// 1 BPM step = 1 quarter-note = 1 clock pulse. So 120 BPM = 2 pulses/sec.
	// Phase accumulator wraps every period; output is high in the first half.
	class FClock : public TNodeBase<0, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Bpm,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Clock"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return {};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Gate", EPortType::Control,
				"Square-wave gate (50% duty cycle) at the configured BPM." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "BPM", 1.0f, 400.0f, 120.0f, false, EParamKind::Float, {},
					"Beats per minute. One pulse per beat." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			return (Index == Param_Bpm) ? Bpm.load(std::memory_order_relaxed) : 0.0f;
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			if (Index == Param_Bpm)
			{
				float V = Value;
				if (V < 1.0f) { V = 1.0f; }
				if (V > 400.0f) { V = 400.0f; }
				Bpm.store(V, std::memory_order_relaxed);
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			Phase = 0.0f;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float BpmNow = Bpm.load(std::memory_order_relaxed);
			const float HzNow = BpmNow / 60.0f;
			const float PhaseInc = HzNow / static_cast<float>(SampleRate);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				Phase += PhaseInc;
				while (Phase >= 1.0f) { Phase -= 1.0f; }
				Out[I] = (Phase < 0.5f) ? 1.0f : 0.0f;
			}
		}

	private:
		std::atomic<float> Bpm{ 120.0f };
		double SampleRate = 48000.0;
		float Phase = 0.0f;
	};
}
