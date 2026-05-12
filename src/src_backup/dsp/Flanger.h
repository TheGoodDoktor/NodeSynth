#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Stereo flanger: short modulated delay (0.5–10 ms) with a feedback
	// path that creates the characteristic comb-filter sweep. L and R LFOs
	// run 90° out of phase. Feedback can be negative (inverts the comb's
	// odd harmonics — emphasises even ones).
	//
	// Same skeleton as FChorus, with three differences:
	//   - delay range is much shorter (≤10 ms vs chorus's ~24 ms)
	//   - has a Feedback param (chorus has none)
	//   - no Voices param (one tap per channel keeps the comb signature
	//     coherent; stacking voices muddies the flange)
	class FFlanger : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Rate,
			Param_Depth,
			Param_Feedback,
			Param_Mix,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Flanger"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to flange." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Dry × (1-Mix) + flanged × Mix, stereo. Negative feedback inverts\n"
				"the comb harmonic emphasis." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Rate",     0.05f, 5.0f,  0.4f, true,  EParamKind::Float, {},
					"LFO rate in Hz. 0.1–0.5 Hz = jet-style slow sweep; 1+ Hz =\n"
					"obvious modulation. Logarithmic." },
				{ "Depth",    0.0f,  1.0f,  0.7f, false, EParamKind::Float, {},
					"Modulation amplitude. 1 = full ±BaseDelay sweep." },
				{ "Feedback", -0.95f, 0.95f, 0.5f, false, EParamKind::Float, {},
					"Comb-filter feedback. Positive emphasises odd harmonics;\n"
					"negative emphasises even (flips the comb). ±0.95 max to\n"
					"prevent runaway." },
				{ "Mix",      0.0f,  1.0f,  0.5f, false, EParamKind::Float, {},
					"Wet/dry blend. 0 = dry passthrough; 1 = pure flanged signal." },
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
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			// Max delay = BaseDelay + Depth * SweepDelay = 5 + 5 = 10 ms.
			// + a few samples for fractional-tap safety.
			Capacity = static_cast<size_t>(InSampleRate * 0.012) + 16;
			Lines[0].Buffer.assign(Capacity, 0.0f);
			Lines[1].Buffer.assign(Capacity, 0.0f);
			Lines[0].WriteIdx = 0;
			Lines[1].WriteIdx = 0;
			LfoPhase = 0.0;

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

			constexpr double TwoPi = 2.0 * 3.141592653589793;
			constexpr double BaseDelayMs = 5.0;     // centre delay in ms
			constexpr double SweepDelayMs = 4.5;     // ±SweepDelayMs * Depth (max ≈ 9.5 ms)

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
				const double DelayMsL = BaseDelayMs + DepthNow * SweepDelayMs * LfoL;
				const double DelayMsR = BaseDelayMs + DepthNow * SweepDelayMs * LfoR;
				const float WetL = TapInterpolated(Lines[0], DelayMsL);
				const float WetR = TapInterpolated(Lines[1], DelayMsR);

				// Write input + feedback into the delay lines (the feedback
				// path is what turns this into a flanger vs a chorus).
				Lines[0].Buffer[Lines[0].WriteIdx] = DryL + FeedbackNow * WetL;
				Lines[1].Buffer[Lines[1].WriteIdx] = DryR + FeedbackNow * WetR;

				OutL[I] = (1.0f - MixNow) * DryL + MixNow * WetL;
				OutR[I] = (1.0f - MixNow) * DryR + MixNow * WetR;

				if (++Lines[0].WriteIdx >= Capacity) { Lines[0].WriteIdx = 0; }
				if (++Lines[1].WriteIdx >= Capacity) { Lines[1].WriteIdx = 0; }
				LfoPhase += TwoPi * RateNow / SampleRate;
				if (LfoPhase >= TwoPi) { LfoPhase -= TwoPi; }
			}
		}

	private:
		struct FLine
		{
			std::vector<float> Buffer;
			size_t WriteIdx = 0;
		};

		float TapInterpolated(const FLine& Line, double DelayMs) const
		{
			double DelaySamples = DelayMs * SampleRate * 0.001;
			if (DelaySamples < 1.0) { DelaySamples = 1.0; }
			if (DelaySamples > static_cast<double>(Capacity - 2))
			{
				DelaySamples = static_cast<double>(Capacity - 2);
			}
			const int32_t Floor = static_cast<int32_t>(DelaySamples);
			const float Frac = static_cast<float>(DelaySamples - Floor);
			const int32_t Cap = static_cast<int32_t>(Capacity);
			const int32_t IdxNewer = (static_cast<int32_t>(Line.WriteIdx) - Floor + Cap) % Cap;
			const int32_t IdxOlder = (static_cast<int32_t>(Line.WriteIdx) - Floor - 1 + Cap) % Cap;
			const float Newer = Line.Buffer[static_cast<size_t>(IdxNewer)];
			const float Older = Line.Buffer[static_cast<size_t>(IdxOlder)];
			return (1.0f - Frac) * Newer + Frac * Older;
		}

		std::atomic<float> Rate{ 0.4f };
		std::atomic<float> Depth{ 0.7f };
		std::atomic<float> Feedback{ 0.5f };
		std::atomic<float> Mix{ 0.5f };

		double SampleRate = 48000.0;
		size_t Capacity = 0;
		FLine Lines[2];
		double LfoPhase = 0.0;
		FOnePoleSmoother RateSmoother;
		FOnePoleSmoother DepthSmoother;
		FOnePoleSmoother FeedbackSmoother;
		FOnePoleSmoother MixSmoother;
	};
}
