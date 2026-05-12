#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Sample-and-hold downsample + bit-depth quantize. Stateless aside from
	// the per-channel S&H counters and held values. Aliasing IS the effect
	// here — that's what users want.
	class FBitcrusher : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_SampleRateRatio,
			Param_Bits,
			Param_Mix,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Bitcrusher"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to crush." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Crushed audio. SampleRateRatio = 1, Bits = 16, Mix = 1 → near-passthrough." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Rate",  0.01f, 1.0f,  1.0f, true,  EParamKind::Float, {},
					"Sample-rate divider. 1 = host rate (no S&H); 0.01 = 1/100th\n"
					"the host rate. Logarithmic." },
				{ "Bits",  1.0f, 16.0f, 16.0f, false, EParamKind::Float, {},
					"Bit depth. 1 = output is one of {-1, +1}; 16 ≈ near-passthrough." },
				{ "Mix",   0.0f,  1.0f,  1.0f, false, EParamKind::Float, {},
					"Wet/dry blend. 0 = dry, 1 = pure crush." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_SampleRateRatio: return SampleRateRatio.load(std::memory_order_relaxed);
				case Param_Bits:            return Bits.load(std::memory_order_relaxed);
				case Param_Mix:             return Mix.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_SampleRateRatio:
				{
					float V = Value;
					if (V < 0.01f) { V = 0.01f; }
					if (V > 1.0f)  { V = 1.0f; }
					SampleRateRatio.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Bits:
				{
					float V = Value;
					if (V < 1.0f)  { V = 1.0f; }
					if (V > 16.0f) { V = 16.0f; }
					Bits.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Mix:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 1.0f) { V = 1.0f; }
					Mix.store(V, std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		void Prepare(double /*SampleRate*/) override
		{
			HoldL = 0.0f; HoldR = 0.0f;
			HoldCounter = 0.0f;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* InL = GetInputBuffer(0, 0);
			const float* InR = GetInputBuffer(0, 1);

			const float Ratio = SampleRateRatio.load(std::memory_order_relaxed);
			const float NumBits = Bits.load(std::memory_order_relaxed);
			const float MixNow = Mix.load(std::memory_order_relaxed);

			// 2^(bits-1) levels per side; total quantizer step = 2 / 2^bits.
			const float Levels = std::pow(2.0f, NumBits) * 0.5f;

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R = (InR != nullptr) ? InR[I] : L;

				// S&H: capture a fresh sample once per (1/Ratio) host samples.
				HoldCounter += Ratio;
				if (HoldCounter >= 1.0f)
				{
					HoldCounter -= 1.0f;
					HoldL = L;
					HoldR = R;
				}

				const float CrushedL = std::round(HoldL * Levels) / Levels;
				const float CrushedR = std::round(HoldR * Levels) / Levels;

				OutL[I] = (1.0f - MixNow) * L + MixNow * CrushedL;
				OutR[I] = (1.0f - MixNow) * R + MixNow * CrushedR;
			}
		}

	private:
		std::atomic<float> SampleRateRatio{ 1.0f };
		std::atomic<float> Bits{ 16.0f };
		std::atomic<float> Mix{ 1.0f };

		float HoldL = 0.0f;
		float HoldR = 0.0f;
		float HoldCounter = 0.0f;
	};
}
