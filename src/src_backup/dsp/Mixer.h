#pragma once

#include <atomic>
#include <cstdint>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// 4-channel audio mixer with per-channel gain. Sums the four (smoothed-gain
	// scaled) audio inputs into a single audio output. Disconnected inputs read
	// as silence. Cloneable, so a per-voice flag layers multiple sources per
	// voice (e.g. `Saw + Square + Sub + Noise → Filter` with the Mixer marked
	// per-voice and the compiler cloning the whole chain).
	class FMixer : public TNodeBase<4, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Gain1,
			Param_Gain2,
			Param_Gain3,
			Param_Gain4,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Mixer"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "In1", EPortType::Audio, "Audio source 1. Multiplied by Gain1." },
				{ "In2", EPortType::Audio, "Audio source 2. Multiplied by Gain2." },
				{ "In3", EPortType::Audio, "Audio source 3. Multiplied by Gain3." },
				{ "In4", EPortType::Audio, "Audio source 4. Multiplied by Gain4." },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Sum of (In1 × Gain1) + (In2 × Gain2) + (In3 × Gain3) + (In4 × Gain4)." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Gain1", 0.0f, 2.0f, 1.0f, false, EParamKind::Float, {},
					"Linear gain for In1. Smoothed." },
				{ "Gain2", 0.0f, 2.0f, 1.0f, false, EParamKind::Float, {},
					"Linear gain for In2. Smoothed." },
				{ "Gain3", 0.0f, 2.0f, 1.0f, false, EParamKind::Float, {},
					"Linear gain for In3. Smoothed." },
				{ "Gain4", 0.0f, 2.0f, 1.0f, false, EParamKind::Float, {},
					"Linear gain for In4. Smoothed." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			if (Index < 4)
			{
				return Gains[Index].load(std::memory_order_relaxed);
			}
			return 0.0f;
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			if (Index < 4)
			{
				float V = Value;
				if (V < 0.0f) { V = 0.0f; }
				if (V > 2.0f) { V = 2.0f; }
				Gains[Index].store(V, std::memory_order_relaxed);
			}
		}

		void Prepare(double InSampleRate) override
		{
			for (size_t I = 0; I < 4; ++I)
			{
				Smoothers[I].Prepare(InSampleRate);
				Smoothers[I].Reset(Gains[I].load(std::memory_order_relaxed));
			}
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* In[4] = {
				GetInputBuffer(0), GetInputBuffer(1),
				GetInputBuffer(2), GetInputBuffer(3),
			};

			for (size_t I = 0; I < 4; ++I)
			{
				Smoothers[I].SetTarget(Gains[I].load(std::memory_order_relaxed));
			}

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				float Sum = 0.0f;
				for (size_t Ch = 0; Ch < 4; ++Ch)
				{
					const float G = Smoothers[Ch].Tick();
					if (In[Ch] != nullptr)
					{
						Sum += In[Ch][I] * G;
					}
				}
				Out[I] = Sum;
			}
		}

	private:
		std::atomic<float> Gains[4]{
			std::atomic<float>{ 1.0f },
			std::atomic<float>{ 1.0f },
			std::atomic<float>{ 1.0f },
			std::atomic<float>{ 1.0f },
		};
		FOnePoleSmoother Smoothers[4];
	};
}
