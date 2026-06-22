#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Envelope.h"
#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Stereo noise gate / downward expander. Uses the same FEnvelopeFollower
	// as compressor / limiter, but the gain curve is flipped: signal *below*
	// the threshold gets attenuated by (threshold - input) × (1 - 1/Ratio).
	// Hold time prevents the gate from chattering on a signal that hovers
	// around the threshold.
	//
	// Linked stereo: detector reads max(|L|, |R|), gain reduction applied
	// identically to both channels.
	class FGate : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_ThresholdDb,
			Param_Ratio,
			Param_AttackMs,
			Param_HoldMs,
			Param_ReleaseMs,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "NoiseGate"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to gate." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Gated audio. Below threshold the gain is reduced; above\n"
				"threshold the signal passes unchanged. Linked stereo." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Threshold", -60.0f,   0.0f, -40.0f, false, EParamKind::Float, {},
					"Level below which gain reduction applies. dBFS." },
				{ "Ratio",      1.0f,  20.0f,  10.0f, false, EParamKind::Float, {},
					"Downward expansion ratio. 10:1 means 1 dB below threshold\n"
					"becomes 10 dB below at the output. Higher ratios approach\n"
					"a hard gate." },
				{ "Attack",     0.1f, 200.0f,   1.0f, true,  EParamKind::Float, {},
					"How fast the gate opens once the signal rises above threshold.\n"
					"Logarithmic." },
				{ "Hold",       0.0f, 500.0f,  10.0f, false, EParamKind::Float, {},
					"After the signal drops below threshold, the gate stays open\n"
					"for this long before starting to close. Prevents chatter\n"
					"on signals that hover around the threshold." },
				{ "Release",    5.0f, 2000.0f, 100.0f, true, EParamKind::Float, {},
					"How fast the gate closes after Hold expires.\n"
					"Logarithmic." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_ThresholdDb: return ThresholdDb.load(std::memory_order_relaxed);
				case Param_Ratio:       return Ratio.load(std::memory_order_relaxed);
				case Param_AttackMs:    return AttackMs.load(std::memory_order_relaxed);
				case Param_HoldMs:      return HoldMs.load(std::memory_order_relaxed);
				case Param_ReleaseMs:   return ReleaseMs.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_ThresholdDb:
				{
					float V = Value;
					if (V < -60.0f) { V = -60.0f; }
					if (V > 0.0f)   { V = 0.0f; }
					ThresholdDb.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Ratio:
				{
					float V = Value;
					if (V < 1.0f)  { V = 1.0f; }
					if (V > 20.0f) { V = 20.0f; }
					Ratio.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_AttackMs:
				{
					float V = Value;
					if (V < 0.1f)   { V = 0.1f; }
					if (V > 200.0f) { V = 200.0f; }
					AttackMs.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_HoldMs:
				{
					float V = Value;
					if (V < 0.0f)   { V = 0.0f; }
					if (V > 500.0f) { V = 500.0f; }
					HoldMs.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_ReleaseMs:
				{
					float V = Value;
					if (V < 5.0f)    { V = 5.0f; }
					if (V > 2000.0f) { V = 2000.0f; }
					ReleaseMs.store(V, std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			Detector.Prepare(InSampleRate);
			HoldSamplesRemaining = 0;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* InL = GetInputBuffer(0, 0);
			const float* InR = GetInputBuffer(0, 1);

			const float ThreshDb = ThresholdDb.load(std::memory_order_relaxed);
			const float RatioNow = Ratio.load(std::memory_order_relaxed);
			Detector.SetTimes(
				AttackMs.load(std::memory_order_relaxed),
				ReleaseMs.load(std::memory_order_relaxed));
			const uint32_t HoldSamplesTotal = static_cast<uint32_t>(
				HoldMs.load(std::memory_order_relaxed) * 0.001f * static_cast<float>(SampleRate));

			const float ThreshLin = DbToLin(ThreshDb);
			const float Slope = 1.0f - 1.0f / RatioNow;  // dB-domain reduction per dB below threshold

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R = (InR != nullptr) ? InR[I] : L;

				const float DetectorIn = std::fmax(std::fabs(L), std::fabs(R));
				const float Env = Detector.Process(DetectorIn);

				// Hold logic: while signal is above threshold, refill the hold
				// counter. While below, count down before letting the gate close.
				if (Env >= ThreshLin)
				{
					HoldSamplesRemaining = HoldSamplesTotal;
				}
				else if (HoldSamplesRemaining > 0)
				{
					--HoldSamplesRemaining;
				}

				float Gain = 1.0f;
				if (Env < ThreshLin && HoldSamplesRemaining == 0 && Env > 0.0f)
				{
					// Same algebra as the compressor's reduction formula but
					// applied below threshold: Gain = (Env / ThreshLin)^Slope
					// which approaches 0 as Env / ThreshLin → 0.
					Gain = std::pow(Env / ThreshLin, Slope);
				}
				OutL[I] = L * Gain;
				OutR[I] = R * Gain;
			}
		}

	private:
		static float DbToLin(float Db) { return std::pow(10.0f, Db * 0.05f); }

		std::atomic<float> ThresholdDb{ -40.0f };
		std::atomic<float> Ratio{ 10.0f };
		std::atomic<float> AttackMs{ 1.0f };
		std::atomic<float> HoldMs{ 10.0f };
		std::atomic<float> ReleaseMs{ 100.0f };

		double SampleRate = 48000.0;
		FEnvelopeFollower Detector;
		uint32_t HoldSamplesRemaining = 0;
	};
}
