#pragma once

#include <atomic>
#include <cmath>

#include "dsp/Node.h"

namespace NodeSynth
{
	class FAdsr : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_AttackMs,
			Param_DecayMs,
			Param_Sustain,
			Param_ReleaseMs,
			Param_COUNT,
		};

		enum class EStage : uint8_t
		{
			Idle,
			Attack,
			Decay,
			Sustain,
			Release,
		};

		const char* GetTypeName() const override { return "ADSR"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Gate", EPortType::Control,
				"Rising edge starts Attack; falling edge starts Release.\n"
				"Threshold 0.5." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Env", EPortType::Control,
				"Envelope output (0..1). Wire into a VCA's Control or an\n"
				"Oscillator's Amp input to shape note loudness." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Attack",  0.1f,   5000.0f, 5.0f,    true,  EParamKind::Float, {},
					"Time in milliseconds for the envelope to rise from 0 to peak after the gate opens." },
				{ "Decay",   1.0f,   5000.0f, 200.0f,  true,  EParamKind::Float, {},
					"Time in milliseconds to fall from peak to the sustain level." },
				{ "Sustain", 0.0f,   1.0f,    0.7f,    false, EParamKind::Float, {},
					"Level held while the gate stays open (0..1)." },
				{ "Release", 1.0f,   8000.0f, 400.0f,  true,  EParamKind::Float, {},
					"Time in milliseconds to fall from the current level to 0 after the gate closes." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_AttackMs:  return AttackMs.load(std::memory_order_relaxed);
				case Param_DecayMs:   return DecayMs.load(std::memory_order_relaxed);
				case Param_Sustain:   return Sustain.load(std::memory_order_relaxed);
				case Param_ReleaseMs: return ReleaseMs.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_AttackMs:  AttackMs.store(Value, std::memory_order_relaxed); break;
				case Param_DecayMs:   DecayMs.store(Value, std::memory_order_relaxed); break;
				case Param_Sustain:   Sustain.store(Value, std::memory_order_relaxed); break;
				case Param_ReleaseMs: ReleaseMs.store(Value, std::memory_order_relaxed); break;
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			Stage = EStage::Idle;
			Level = 0.0f;
			bPrevGateHigh = false;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* Gate = GetInputBuffer(0);

			const float SustainLevel = Sustain.load(std::memory_order_relaxed);
			const float AttackStep = ComputeStep(AttackMs.load(std::memory_order_relaxed));
			const float DecayStep  = ComputeStep(DecayMs.load(std::memory_order_relaxed));
			const float ReleaseStep = ComputeStep(ReleaseMs.load(std::memory_order_relaxed));

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const bool bGateHigh = (Gate != nullptr) && (Gate[I] > 0.5f);

				// Rising edge — retrigger. Attack starts from the current level so
				// re-trigger during release doesn't click.
				if (bGateHigh && !bPrevGateHigh)
				{
					Stage = EStage::Attack;
				}
				else if (!bGateHigh && bPrevGateHigh)
				{
					Stage = EStage::Release;
				}
				bPrevGateHigh = bGateHigh;

				switch (Stage)
				{
					case EStage::Attack:
						Level += AttackStep * (1.0f - Level);
						if (Level >= 0.999f)
						{
							Level = 1.0f;
							Stage = EStage::Decay;
						}
						break;
					case EStage::Decay:
						Level += DecayStep * (SustainLevel - Level);
						if (std::fabs(Level - SustainLevel) < 1e-4f)
						{
							Level = SustainLevel;
							Stage = EStage::Sustain;
						}
						break;
					case EStage::Sustain:
						Level = SustainLevel;
						break;
					case EStage::Release:
						Level += ReleaseStep * (0.0f - Level);
						if (Level < 1e-4f)
						{
							Level = 0.0f;
							Stage = EStage::Idle;
						}
						break;
					case EStage::Idle:
					default:
						Level = 0.0f;
						break;
				}

				Out[I] = Level;
			}
		}

		EStage GetStage() const { return Stage; }

	private:
		// Per-sample coefficient for an exponential approach to a target over ~TimeMs.
		// 1 - exp(-1 / (SampleRate * TimeMs / 1000)) gives the per-sample fraction of
		// remaining distance to cover. At TimeMs = 0 we return 1 (instant).
		float ComputeStep(float TimeMs) const
		{
			if (TimeMs <= 0.0f)
			{
				return 1.0f;
			}
			const double Samples = (static_cast<double>(TimeMs) * 0.001) * SampleRate;
			if (Samples <= 0.0)
			{
				return 1.0f;
			}
			return static_cast<float>(1.0 - std::exp(-1.0 / Samples));
		}

		std::atomic<float> AttackMs{ 5.0f };
		std::atomic<float> DecayMs{ 200.0f };
		std::atomic<float> Sustain{ 0.7f };
		std::atomic<float> ReleaseMs{ 400.0f };

		double SampleRate = 48000.0;
		EStage Stage = EStage::Idle;
		float Level = 0.0f;
		bool bPrevGateHigh = false;
	};
}
