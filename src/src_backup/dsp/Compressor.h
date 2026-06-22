#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Envelope.h"
#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Stereo compressor with linked detection: max(|L|, |R|) feeds a peak
	// envelope follower; the resulting gain reduction applies identically
	// to both channels so the stereo image stays intact when one side is
	// briefly louder than the other (standard hardware-comp behaviour).
	//
	// Hard-knee, no lookahead. Adequate for synth use where transients
	// are envelope-driven and predictable. A lookahead limiter is a
	// future enhancement gated on graph-wide latency compensation.
	class FCompressor : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_ThresholdDb,
			Param_Ratio,
			Param_AttackMs,
			Param_ReleaseMs,
			Param_MakeupGainDb,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Compressor"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to compress." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Compressed audio. Gain reduction is linked stereo —\n"
				"both channels reduce by the same amount so the image\n"
				"stays intact." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Threshold",  -60.0f,  0.0f,  -12.0f, false, EParamKind::Float, {},
					"Level above which gain reduction kicks in. dBFS." },
				{ "Ratio",       1.0f,  20.0f,   4.0f,  false, EParamKind::Float, {},
					"Compression ratio. 4:1 means 4 dB above threshold becomes\n"
					"1 dB above threshold at the output." },
				{ "Attack",      0.1f, 200.0f,  10.0f,  true,  EParamKind::Float, {},
					"Time constant for the envelope follower's rising response.\n"
					"Logarithmic. Shorter = catches transients faster." },
				{ "Release",     5.0f, 2000.0f, 150.0f, true,  EParamKind::Float, {},
					"Time constant for the envelope follower's falling response.\n"
					"Logarithmic. Shorter = punchier, longer = smoother." },
				{ "Makeup",      0.0f,  24.0f,   0.0f,  false, EParamKind::Float, {},
					"Post-gain in dB to compensate for the level reduction\n"
					"caused by compression." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_ThresholdDb:  return ThresholdDb.load(std::memory_order_relaxed);
				case Param_Ratio:        return Ratio.load(std::memory_order_relaxed);
				case Param_AttackMs:     return AttackMs.load(std::memory_order_relaxed);
				case Param_ReleaseMs:    return ReleaseMs.load(std::memory_order_relaxed);
				case Param_MakeupGainDb: return MakeupGainDb.load(std::memory_order_relaxed);
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
				case Param_ReleaseMs:
				{
					float V = Value;
					if (V < 5.0f)    { V = 5.0f; }
					if (V > 2000.0f) { V = 2000.0f; }
					ReleaseMs.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_MakeupGainDb:
				{
					float V = Value;
					if (V < 0.0f)  { V = 0.0f; }
					if (V > 24.0f) { V = 24.0f; }
					MakeupGainDb.store(V, std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			Detector.Prepare(InSampleRate);
			MakeupSmoother.Prepare(InSampleRate, 30.0f);
			MakeupSmoother.Reset(DbToLin(MakeupGainDb.load(std::memory_order_relaxed)));
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
			MakeupSmoother.SetTarget(DbToLin(MakeupGainDb.load(std::memory_order_relaxed)));

			const float ThreshLin = DbToLin(ThreshDb);
			const float Slope = 1.0f - 1.0f / RatioNow;  // dB-domain reduction per dB over threshold

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R = (InR != nullptr) ? InR[I] : L;

				// Linked stereo: detector reads the larger of |L| and |R|.
				const float DetectorIn = std::fmax(std::fabs(L), std::fabs(R));
				const float Env = Detector.Process(DetectorIn);

				const float MakeupLin = MakeupSmoother.Tick();
				float Gain = MakeupLin;
				if (Env > ThreshLin)
				{
					// (Env / ThreshLin)^(-Slope) computes the reduction factor
					// without round-tripping through dB. Equivalent to
					// 10^(-Slope * 20*log10(Env/ThreshLin) / 20).
					Gain = MakeupLin * std::pow(Env / ThreshLin, -Slope);
				}
				OutL[I] = L * Gain;
				OutR[I] = R * Gain;
			}
		}

	private:
		static float DbToLin(float Db)
		{
			return std::pow(10.0f, Db * 0.05f);
		}

		std::atomic<float> ThresholdDb{ -12.0f };
		std::atomic<float> Ratio{ 4.0f };
		std::atomic<float> AttackMs{ 10.0f };
		std::atomic<float> ReleaseMs{ 150.0f };
		std::atomic<float> MakeupGainDb{ 0.0f };

		FEnvelopeFollower Detector;
		FOnePoleSmoother MakeupSmoother;
	};
}
