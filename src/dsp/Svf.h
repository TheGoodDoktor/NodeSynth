#pragma once

#include <atomic>
#include <cmath>
#include <numbers>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Linear trapezoidal state-variable filter (Zavalishin / Simper TPT form).
	// Stable across the full cutoff × resonance range including self-oscillation.
	//
	// Three inputs: Audio, Cutoff (Hz), Resonance (0..1). Three outputs: LP, HP, BP.
	// Disconnected Cutoff/Resonance inputs fall back to the param slider value.
	class FSvf : public TNodeBase<3, 3>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Cutoff,
			Param_Resonance,
			Param_COUNT,
		};

		enum EInput : uint32_t
		{
			Input_Audio,
			Input_Cutoff,
			Input_Resonance,
		};

		enum EOutput : uint32_t
		{
			Output_LowPass,
			Output_HighPass,
			Output_BandPass,
		};

		const char* GetTypeName() const override { return "SVF"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "Audio",     EPortType::Audio },
				{ "Cutoff",    EPortType::Control },
				{ "Resonance", EPortType::Control },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return
			{
				{ "LP", EPortType::Audio },
				{ "HP", EPortType::Audio },
				{ "BP", EPortType::Audio },
			};
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Cutoff",    20.0f, 20000.0f, 1000.0f, true,  EParamKind::Float, {} },
				{ "Resonance", 0.0f,  1.0f,     0.2f,   false, EParamKind::Float, {} },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Cutoff:    return Cutoff.load(std::memory_order_relaxed);
				case Param_Resonance: return Resonance.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Cutoff:    Cutoff.store(Value, std::memory_order_relaxed); break;
				case Param_Resonance: Resonance.store(Value, std::memory_order_relaxed); break;
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			Ic1Eq = 0.0f;
			Ic2Eq = 0.0f;
		}

		void Process(const FProcessContext& Ctx) override
		{
			const float* Audio = GetInputBuffer(Input_Audio);
			const float* CutoffBuf = GetInputBuffer(Input_Cutoff);
			const float* ResBuf = GetInputBuffer(Input_Resonance);

			float* LP = GetOutputBuffer(Output_LowPass);
			float* HP = GetOutputBuffer(Output_HighPass);
			float* BP = GetOutputBuffer(Output_BandPass);

			if (Audio == nullptr)
			{
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					LP[I] = 0.0f;
					HP[I] = 0.0f;
					BP[I] = 0.0f;
				}
				return;
			}

			const float CutoffParam = Cutoff.load(std::memory_order_relaxed);
			const float ResParam = Resonance.load(std::memory_order_relaxed);
			const float CutoffMax = static_cast<float>(0.49 * SampleRate);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				float CutoffHz = (CutoffBuf != nullptr) ? CutoffBuf[I] : CutoffParam;
				if (CutoffHz < 20.0f)
				{
					CutoffHz = 20.0f;
				}
				if (CutoffHz > CutoffMax)
				{
					CutoffHz = CutoffMax;
				}
				float Res = (ResBuf != nullptr) ? ResBuf[I] : ResParam;
				if (Res < 0.0f)
				{
					Res = 0.0f;
				}
				if (Res > 1.0f)
				{
					Res = 1.0f;
				}

				// TPT coefficients: g = tan(pi * fc / fs), k = 2 * (1 - res) gives
				// k=2 at res=0 (heavy damping), k=0 at res=1 (self-oscillation).
				const float G = static_cast<float>(std::tan(std::numbers::pi * CutoffHz / SampleRate));
				const float K = 2.0f * (1.0f - Res);
				const float A1 = 1.0f / (1.0f + G * (G + K));
				const float A2 = G * A1;
				const float A3 = G * A2;

				const float V0 = Audio[I];
				const float V3 = V0 - Ic2Eq;
				const float V1 = A1 * Ic1Eq + A2 * V3;
				const float V2 = Ic2Eq + A2 * Ic1Eq + A3 * V3;
				Ic1Eq = 2.0f * V1 - Ic1Eq;
				Ic2Eq = 2.0f * V2 - Ic2Eq;

				LP[I] = V2;
				BP[I] = V1;
				HP[I] = V0 - K * V1 - V2;
			}
		}

	private:
		std::atomic<float> Cutoff{ 1000.0f };
		std::atomic<float> Resonance{ 0.2f };
		double SampleRate = 48000.0;
		float Ic1Eq = 0.0f;
		float Ic2Eq = 0.0f;
	};
}
