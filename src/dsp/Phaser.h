#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Stereo all-pass-cascade phaser. Different mechanism from chorus and
	// flanger: instead of modulating a delay line, phaser modulates the
	// corner frequency of N cascaded 1st-order all-pass filters. Cascade +
	// dry-sum produces N/2 notches in the spectrum that sweep up and down
	// with the LFO.
	//
	// Two parallel cascades (one per channel), L/R LFOs 90° out of phase
	// for stereo width. Mono input fans out to both channels; stereo input
	// processes channel-paired.
	class FPhaser : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Rate,
			Param_Depth,
			Param_Feedback,
			Param_Mix,
			Param_Stages,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Phaser"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to phase." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Dry × (1-Mix) + phased × Mix, stereo. L and R LFOs are 90°\n"
				"out of phase so a mono input still produces stereo width." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Rate",     0.05f,  5.0f,  0.4f,  true,  EParamKind::Float, {},
					"LFO rate in Hz. 0.1–0.5 Hz = slow, vocal-y sweep; >1 Hz starts\n"
					"to sound like vibrato. Logarithmic." },
				{ "Depth",    0.0f,   1.0f,  0.7f,  false, EParamKind::Float, {},
					"Modulation amount. 1 = ±2 octave sweep around 800 Hz." },
				{ "Feedback", -0.95f, 0.95f, 0.5f,  false, EParamKind::Float, {},
					"Notch-emphasis feedback. Positive sharpens; negative inverts\n"
					"the alternating pattern (Phase 90's sharper vocal sound)." },
				{ "Mix",      0.0f,   1.0f,  0.5f,  false, EParamKind::Float, {},
					"Wet/dry blend. 0 = dry passthrough; 1 = pure wet." },
				{ "Stages",   0.0f,   2.0f,  1.0f,  false, EParamKind::Choice,
					{ "4", "6", "8" },
					"Number of all-pass stages in the cascade. 4 = subtle, 6 =\n"
					"classic Phase 90, 8 = thicker." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Rate:     return Rate.load(std::memory_order_relaxed);
				case Param_Depth:    return Depth.load(std::memory_order_relaxed);
				case Param_Feedback: return Feedback.load(std::memory_order_relaxed);
				case Param_Mix:      return Mix.load(std::memory_order_relaxed);
				case Param_Stages:   return static_cast<float>(StagesIdx.load(std::memory_order_relaxed));
				default: return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Rate:
				{
					float V = Value;
					if (V < 0.05f) { V = 0.05f; }
					if (V > 5.0f) { V = 5.0f; }
					Rate.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Depth:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 1.0f) { V = 1.0f; }
					Depth.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Feedback:
				{
					float V = Value;
					if (V < -0.95f) { V = -0.95f; }
					if (V > 0.95f) { V = 0.95f; }
					Feedback.store(V, std::memory_order_relaxed);
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
				case Param_Stages:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0) { V = 0; }
					if (V > 2) { V = 2; }
					StagesIdx.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			LfoPhase = 0.0;
			FeedbackTap[0] = 0.0f;
			FeedbackTap[1] = 0.0f;
			for (uint32_t I = 0; I < MaxStages; ++I)
			{
				StagesL[I].Reset();
				StagesR[I].Reset();
			}

			RateSmoother.Prepare(InSampleRate, 30.0f);
			DepthSmoother.Prepare(InSampleRate, 30.0f);
			FeedbackSmoother.Prepare(InSampleRate, 30.0f);
			MixSmoother.Prepare(InSampleRate, 30.0f);
			RateSmoother.Reset(Rate.load(std::memory_order_relaxed));
			DepthSmoother.Reset(Depth.load(std::memory_order_relaxed));
			FeedbackSmoother.Reset(Feedback.load(std::memory_order_relaxed));
			MixSmoother.Reset(Mix.load(std::memory_order_relaxed));
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* InL = GetInputBuffer(0, 0);
			const float* InR = GetInputBuffer(0, 1);

			RateSmoother.SetTarget(Rate.load(std::memory_order_relaxed));
			DepthSmoother.SetTarget(Depth.load(std::memory_order_relaxed));
			FeedbackSmoother.SetTarget(Feedback.load(std::memory_order_relaxed));
			MixSmoother.SetTarget(Mix.load(std::memory_order_relaxed));
			const uint32_t NumStages = StagesPerChoice(StagesIdx.load(std::memory_order_relaxed));

			constexpr double TwoPi = 2.0 * 3.141592653589793;
			constexpr float CentreHz = 800.0f;       // sweep centre, geometric mean of musical range
			constexpr float MaxOctaves = 2.0f;        // ±2 octaves at Depth = 1
			const float SrF = static_cast<float>(SampleRate);
			const float MinFc = 20.0f;
			const float MaxFc = 0.45f * SrF;          // stay below Nyquist

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float RateNow = RateSmoother.Tick();
				const float DepthNow = DepthSmoother.Tick();
				const float FeedbackNow = FeedbackSmoother.Tick();
				const float MixNow = MixSmoother.Tick();

				const float DryL = (InL != nullptr) ? InL[I] : 0.0f;
				const float DryR = (InR != nullptr) ? InR[I] : DryL;

				const double LfoL = std::sin(LfoPhase);
				const double LfoR = std::sin(LfoPhase + 3.141592653589793 * 0.5);

				const float FcL = ClampFc(CentreHz * std::pow(2.0f, MaxOctaves * DepthNow * static_cast<float>(LfoL)),
					MinFc, MaxFc);
				const float FcR = ClampFc(CentreHz * std::pow(2.0f, MaxOctaves * DepthNow * static_cast<float>(LfoR)),
					MinFc, MaxFc);
				const float CoeffL = AllpassCoeff(FcL, SrF);
				const float CoeffR = AllpassCoeff(FcR, SrF);

				// Cascade L: input + feedback → all-pass × N → wet sample.
				float WetL = DryL + FeedbackNow * FeedbackTap[0];
				for (uint32_t S = 0; S < NumStages; ++S)
				{
					WetL = StagesL[S].Process(WetL, CoeffL);
				}
				FeedbackTap[0] = WetL;

				float WetR = DryR + FeedbackNow * FeedbackTap[1];
				for (uint32_t S = 0; S < NumStages; ++S)
				{
					WetR = StagesR[S].Process(WetR, CoeffR);
				}
				FeedbackTap[1] = WetR;

				OutL[I] = (1.0f - MixNow) * DryL + MixNow * WetL;
				OutR[I] = (1.0f - MixNow) * DryR + MixNow * WetR;

				LfoPhase += TwoPi * RateNow / SampleRate;
				if (LfoPhase >= TwoPi) { LfoPhase -= TwoPi; }
			}
		}

	private:
		// 1st-order all-pass: y[n] = -a * x[n] + z; z = x[n] + a * y[n].
		// Single-sample state, transfer fn (a + z⁻¹) / (1 + a·z⁻¹). At the
		// corner frequency, introduces 90° phase shift with unity magnitude
		// — that's the whole point.
		struct FAllpass1stOrder
		{
			float Z = 0.0f;
			float Process(float X, float A)
			{
				const float Y = -A * X + Z;
				Z = X + A * Y;
				return Y;
			}
			void Reset() { Z = 0.0f; }
		};

		static float AllpassCoeff(float Fc, float Sr)
		{
			const float T = std::tan(static_cast<float>(3.141592653589793) * Fc / Sr);
			return (1.0f - T) / (1.0f + T);
		}

		static float ClampFc(float Fc, float Min, float Max)
		{
			if (Fc < Min) { return Min; }
			if (Fc > Max) { return Max; }
			return Fc;
		}

		static uint32_t StagesPerChoice(uint8_t Idx)
		{
			// 0 → 4, 1 → 6, 2 → 8.
			return 4u + 2u * (Idx > 2 ? 2u : Idx);
		}

		static constexpr uint32_t MaxStages = 8;

		std::atomic<float>   Rate{ 0.4f };
		std::atomic<float>   Depth{ 0.7f };
		std::atomic<float>   Feedback{ 0.5f };
		std::atomic<float>   Mix{ 0.5f };
		std::atomic<uint8_t> StagesIdx{ 1 };  // 0=4 stages, 1=6 stages, 2=8 stages

		double SampleRate = 48000.0;
		double LfoPhase = 0.0;
		float FeedbackTap[2] = { 0.0f, 0.0f };

		FAllpass1stOrder StagesL[MaxStages];
		FAllpass1stOrder StagesR[MaxStages];

		FOnePoleSmoother RateSmoother;
		FOnePoleSmoother DepthSmoother;
		FOnePoleSmoother FeedbackSmoother;
		FOnePoleSmoother MixSmoother;
	};
}
