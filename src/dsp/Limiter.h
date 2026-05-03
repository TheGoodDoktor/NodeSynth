#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Envelope.h"
#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Stereo brickwall-style limiter — a compressor with infinite ratio,
	// hard knee, and a tighter default attack. No lookahead in v1, so very
	// fast transients can briefly slip past the ceiling — adequate for
	// synth use, where transients are envelope-driven and predictable.
	// A proper lookahead limiter is gated on graph-wide latency
	// compensation (see PLAN-EFFECTS-ROADMAP §1.5).
	//
	// Linked stereo detection: detector reads max(|L|, |R|), gain reduction
	// applied identically to both channels.
	class FLimiter : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_CeilingDb,
			Param_ReleaseMs,
			Param_MakeupGainDb,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Limiter"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to limit." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Limited audio. Ceiling is a hard cap on output amplitude\n"
				"(linked stereo). Attack is fixed-fast; no lookahead in v1." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Ceiling",  -24.0f,   0.0f,  -0.3f, false, EParamKind::Float, {},
					"Output ceiling in dBFS. Default -0.3 dB so the limiter\n"
					"prevents clipping at the device output." },
				{ "Release",   5.0f, 2000.0f, 100.0f, true,  EParamKind::Float, {},
					"Time constant for the envelope follower's falling response.\n"
					"Logarithmic. Shorter = punchier, longer = smoother." },
				{ "Makeup",    0.0f,  24.0f,    0.0f, false, EParamKind::Float, {},
					"Post-gain in dB. Lift the level back up after limiting." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_CeilingDb:    return CeilingDb.load(std::memory_order_relaxed);
				case Param_ReleaseMs:    return ReleaseMs.load(std::memory_order_relaxed);
				case Param_MakeupGainDb: return MakeupGainDb.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_CeilingDb:
				{
					float V = Value;
					if (V < -24.0f) { V = -24.0f; }
					if (V > 0.0f)   { V = 0.0f; }
					CeilingDb.store(V, std::memory_order_relaxed);
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

			// Fixed-fast attack (1 ms) — limiter convention. Release is
			// user-controlled.
			Detector.SetTimes(1.0f, ReleaseMs.load(std::memory_order_relaxed));
			MakeupSmoother.SetTarget(DbToLin(MakeupGainDb.load(std::memory_order_relaxed)));

			const float CeilingLin = DbToLin(CeilingDb.load(std::memory_order_relaxed));

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R = (InR != nullptr) ? InR[I] : L;

				const float DetectorIn = std::fmax(std::fabs(L), std::fabs(R));
				const float Env = Detector.Process(DetectorIn);

				const float MakeupLin = MakeupSmoother.Tick();
				// Hard ceiling: gain = ceiling / env when above. Below
				// ceiling, gain = 1 (just makeup).
				float Gain = MakeupLin;
				if (Env > CeilingLin && Env > 0.0f)
				{
					Gain = MakeupLin * (CeilingLin / Env);
				}
				OutL[I] = L * Gain;
				OutR[I] = R * Gain;
			}
		}

	private:
		static float DbToLin(float Db) { return std::pow(10.0f, Db * 0.05f); }

		std::atomic<float> CeilingDb{ -0.3f };
		std::atomic<float> ReleaseMs{ 100.0f };
		std::atomic<float> MakeupGainDb{ 0.0f };

		FEnvelopeFollower Detector;
		FOnePoleSmoother MakeupSmoother;
	};
}
