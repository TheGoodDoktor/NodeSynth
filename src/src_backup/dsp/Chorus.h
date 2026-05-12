#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Stereo chorus: two delay lines (one per channel) with their tap
	// positions modulated by an LFO. The L and R LFOs run 90° out of phase
	// for stereo width. Optionally stacks N "voices" at 360°/N phase
	// offsets within each channel for a thicker sound. No feedback path
	// (that's flanger territory).
	//
	// Mono input is broadcast to both channels (the Phase 5b convention).
	// Stereo input is processed channel-paired.
	class FChorus : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Rate,    // LFO rate Hz
			Param_Depth,   // 0..1 — modulation amplitude
			Param_Mix,     // 0..1 — wet/dry blend
			Param_Voices,  // Choice 1/2/3 — stacked LFO-offset taps per channel
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Chorus"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to chorus." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Dry × (1-Mix) + chorused × Mix, stereo. L and R LFOs are\n"
				"90° out of phase so a mono input still produces stereo width." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Rate",   0.05f, 10.0f, 0.6f, true, EParamKind::Float, {},
					"LFO rate in Hz. 0.5–1 Hz = classic chorus shimmer; >3 Hz starts\n"
					"to sound vibrato-y. Logarithmic." },
				{ "Depth",  0.0f, 1.0f, 0.5f, false, EParamKind::Float, {},
					"Modulation amplitude. 0 = no modulation (just a fixed short delay);\n"
					"1 = full ±BaseDelay sweep." },
				{ "Mix",    0.0f, 1.0f, 0.5f, false, EParamKind::Float, {},
					"Wet/dry blend. 0 = dry passthrough; 1 = pure chorused signal." },
				{ "Voices", 0.0f, 2.0f, 0.0f, false, EParamKind::Choice,
					{ "1", "2", "3" },
					"Number of stacked LFO-offset taps per channel. More voices =\n"
					"thicker, denser chorus at the cost of CPU." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Rate:   return Rate.load(std::memory_order_relaxed);
				case Param_Depth:  return Depth.load(std::memory_order_relaxed);
				case Param_Mix:    return Mix.load(std::memory_order_relaxed);
				case Param_Voices: return static_cast<float>(VoicesIdx.load(std::memory_order_relaxed));
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
					if (V > 10.0f) { V = 10.0f; }
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
				case Param_Mix:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 1.0f) { V = 1.0f; }
					Mix.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Voices:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0) { V = 0; }
					if (V > 2) { V = 2; }
					VoicesIdx.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			// Buffer sized for max delay = BaseDelay + Depth * BaseDelay = 2 * BaseDelay
			// = 24 ms. Round up + a few samples for fractional-tap safety.
			Capacity = static_cast<size_t>(InSampleRate * 0.030) + 16;
			Lines[0].Buffer.assign(Capacity, 0.0f);
			Lines[1].Buffer.assign(Capacity, 0.0f);
			Lines[0].WriteIdx = 0;
			Lines[1].WriteIdx = 0;
			LfoPhase = 0.0;

			RateSmoother.Prepare(InSampleRate, 30.0f);
			DepthSmoother.Prepare(InSampleRate, 30.0f);
			MixSmoother.Prepare(InSampleRate, 30.0f);
			RateSmoother.Reset(Rate.load(std::memory_order_relaxed));
			DepthSmoother.Reset(Depth.load(std::memory_order_relaxed));
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
			MixSmoother.SetTarget(Mix.load(std::memory_order_relaxed));
			const int32_t NumVoices = static_cast<int32_t>(VoicesIdx.load(std::memory_order_relaxed)) + 1;

			constexpr double TwoPi = 2.0 * 3.141592653589793;
			constexpr double BaseDelayMs = 12.0;        // centre delay in ms
			constexpr double SweepDelayMs = 8.0;         // ±SweepDelayMs * Depth

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float RateNow = RateSmoother.Tick();
				const float DepthNow = DepthSmoother.Tick();
				const float MixNow = MixSmoother.Tick();

				const float DryL = (InL != nullptr) ? InL[I] : 0.0f;
				const float DryR = (InR != nullptr) ? InR[I] : DryL;

				// Write current input into both delay lines.
				Lines[0].Buffer[Lines[0].WriteIdx] = DryL;
				Lines[1].Buffer[Lines[1].WriteIdx] = DryR;

				float WetL = 0.0f;
				float WetR = 0.0f;
				for (int32_t V = 0; V < NumVoices; ++V)
				{
					// Voice phase offsets: 0°, 120°, 240° within the channel.
					const double VoiceOffset = (2.0 * 3.141592653589793 / NumVoices) * V;
					const double PhaseL = LfoPhase + VoiceOffset;
					const double PhaseR = LfoPhase + VoiceOffset + (3.141592653589793 * 0.5);
					const double LfoL = std::sin(PhaseL);
					const double LfoR = std::sin(PhaseR);
					const double DelayMsL = BaseDelayMs + DepthNow * SweepDelayMs * LfoL;
					const double DelayMsR = BaseDelayMs + DepthNow * SweepDelayMs * LfoR;
					WetL += TapInterpolated(Lines[0], DelayMsL);
					WetR += TapInterpolated(Lines[1], DelayMsR);
				}
				const float VoiceScale = 1.0f / static_cast<float>(NumVoices);
				WetL *= VoiceScale;
				WetR *= VoiceScale;

				OutL[I] = (1.0f - MixNow) * DryL + MixNow * WetL;
				OutR[I] = (1.0f - MixNow) * DryR + MixNow * WetR;

				// Advance write indices and LFO phase.
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

		// Read a fractional-sample tap from a line, going DelayMs back from
		// WriteIdx with linear interpolation. Defensive against tiny delays
		// (clamps to 1 sample) and capacity overruns.
		float TapInterpolated(const FLine& Line, double DelayMs) const
		{
			double DelaySamples = DelayMs * SampleRate * 0.001;
			if (DelaySamples < 1.0) { DelaySamples = 1.0; }
			if (DelaySamples > static_cast<double>(Capacity - 2)) { DelaySamples = static_cast<double>(Capacity - 2); }
			const int32_t Floor = static_cast<int32_t>(DelaySamples);
			const float Frac = static_cast<float>(DelaySamples - Floor);
			const int32_t Cap = static_cast<int32_t>(Capacity);
			const int32_t IdxNewer = (static_cast<int32_t>(Line.WriteIdx) - Floor + Cap) % Cap;
			const int32_t IdxOlder = (static_cast<int32_t>(Line.WriteIdx) - Floor - 1 + Cap) % Cap;
			const float Newer = Line.Buffer[static_cast<size_t>(IdxNewer)];
			const float Older = Line.Buffer[static_cast<size_t>(IdxOlder)];
			return (1.0f - Frac) * Newer + Frac * Older;
		}

		std::atomic<float>   Rate{ 0.6f };
		std::atomic<float>   Depth{ 0.5f };
		std::atomic<float>   Mix{ 0.5f };
		std::atomic<uint8_t> VoicesIdx{ 0 };  // 0=1, 1=2, 2=3 voices

		double SampleRate = 48000.0;
		size_t Capacity = 0;
		FLine Lines[2];
		double LfoPhase = 0.0;
		FOnePoleSmoother RateSmoother;
		FOnePoleSmoother DepthSmoother;
		FOnePoleSmoother MixSmoother;
	};
}
