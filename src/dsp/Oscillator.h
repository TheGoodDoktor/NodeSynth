#pragma once

#include <atomic>
#include <cmath>
#include <numbers>

#include "dsp/Node.h"

namespace NodeSynth
{
	class FOscillator : public TNodeBase<0, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Frequency,
			Param_Amplitude,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Oscillator"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return {};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Frequency", 20.0f, 20000.0f, 440.0f, true  },
				{ "Amplitude", 0.0f,  1.0f,     0.3f,   false },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Frequency: return Frequency.load(std::memory_order_relaxed);
				case Param_Amplitude: return Amplitude.load(std::memory_order_relaxed);
				default:              return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Frequency: Frequency.store(Value, std::memory_order_relaxed); break;
				case Param_Amplitude: Amplitude.store(Value, std::memory_order_relaxed); break;
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const double Freq = static_cast<double>(Frequency.load(std::memory_order_relaxed));
			const float Amp = Amplitude.load(std::memory_order_relaxed);
			const double TwoPi = std::numbers::pi * 2.0;
			const double PhaseInc = TwoPi * Freq / SampleRate;

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				Out[I] = Amp * static_cast<float>(std::sin(Phase));
				Phase += PhaseInc;
				if (Phase >= TwoPi)
				{
					Phase -= TwoPi;
				}
			}
		}

	private:
		std::atomic<float> Frequency{ 440.0f };
		std::atomic<float> Amplitude{ 0.3f };
		double Phase = 0.0;
		double SampleRate = 48000.0;
	};
}
