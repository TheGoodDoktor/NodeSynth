#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Stereo tremolo: LFO modulating amplitude. Simpler than chorus / flanger
	// (no delay line). Stereo modes — Mono ties L and R to the same LFO,
	// Quad runs them 180° out of phase for a "ping-pong" tremolo.
	class FTremolo : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Rate,
			Param_Depth,
			Param_Shape,    // Sine / Triangle / Square / Saw
			Param_Stereo,   // Mono / Quad (180° L/R offset)
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Tremolo"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to tremolo." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Amplitude-modulated audio. Depth 0 = bit-identical passthrough." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Rate",   0.05f, 20.0f, 4.0f, true,  EParamKind::Float, {},
					"LFO rate in Hz. 4 Hz = classic guitar tremolo; >10 Hz = ring-mod-y.\n"
					"Logarithmic." },
				{ "Depth",  0.0f, 1.0f,  0.5f, false, EParamKind::Float, {},
					"Modulation depth. 0 = no modulation (passthrough); 1 = full\n"
					"amplitude swing from 0 to input level." },
				{ "Shape",  0.0f, 3.0f,  0.0f, false, EParamKind::Choice,
					{ "Sine", "Triangle", "Square", "Saw" },
					"LFO waveform. Sine is smooth; Square is on/off chopping." },
				{ "Stereo", 0.0f, 1.0f,  0.0f, false, EParamKind::Choice,
					{ "Mono", "Quad" },
					"Mono = L and R modulated identically; Quad = L and R 180° apart\n"
					"(ping-pong amplitude — when L is loud, R is quiet)." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Rate:   return Rate.load(std::memory_order_relaxed);
				case Param_Depth:  return Depth.load(std::memory_order_relaxed);
				case Param_Shape:  return static_cast<float>(ShapeIdx.load(std::memory_order_relaxed));
				case Param_Stereo: return static_cast<float>(StereoIdx.load(std::memory_order_relaxed));
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
					if (V > 20.0f) { V = 20.0f; }
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
				case Param_Shape:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0) { V = 0; }
					if (V > 3) { V = 3; }
					ShapeIdx.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				case Param_Stereo:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0) { V = 0; }
					if (V > 1) { V = 1; }
					StereoIdx.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			LfoPhase = 0.0;
			RateSmoother.Prepare(InSampleRate, 30.0f);
			DepthSmoother.Prepare(InSampleRate, 30.0f);
			RateSmoother.Reset(Rate.load(std::memory_order_relaxed));
			DepthSmoother.Reset(Depth.load(std::memory_order_relaxed));
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* InL = GetInputBuffer(0, 0);
			const float* InR = GetInputBuffer(0, 1);

			RateSmoother.SetTarget(Rate.load(std::memory_order_relaxed));
			DepthSmoother.SetTarget(Depth.load(std::memory_order_relaxed));
			const uint8_t Shape = ShapeIdx.load(std::memory_order_relaxed);
			const bool bQuad = (StereoIdx.load(std::memory_order_relaxed) != 0);

			constexpr double TwoPi = 2.0 * 3.141592653589793;
			constexpr float Pi = 3.141592653589793f;

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float RateNow = RateSmoother.Tick();
				const float DepthNow = DepthSmoother.Tick();

				// LFO sample for L is computed at LfoPhase. R is offset by π
				// in Quad mode (180° = ping-pong tremolo).
				const float LfoL = LfoSample(Shape, static_cast<float>(LfoPhase));
				const float LfoR = bQuad
					? LfoSample(Shape, static_cast<float>(LfoPhase) + Pi)
					: LfoL;
				// LFO is in [-1, +1]; map to gain in [1 - depth, 1] so depth=0
				// is identity and depth=1 ranges from 0 to 1 (full-swing AM).
				const float GainL = 1.0f - DepthNow + DepthNow * (LfoL * 0.5f + 0.5f);
				const float GainR = 1.0f - DepthNow + DepthNow * (LfoR * 0.5f + 0.5f);

				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R = (InR != nullptr) ? InR[I] : L;
				OutL[I] = L * GainL;
				OutR[I] = R * GainR;

				LfoPhase += TwoPi * RateNow / SampleRate;
				if (LfoPhase >= TwoPi) { LfoPhase -= TwoPi; }
			}
		}

	private:
		// LFO at given phase (radians, wrapped to [0, 2π)) for the chosen
		// shape. Returns a value in [-1, 1].
		static float LfoSample(uint8_t Shape, float Phase)
		{
			constexpr float Pi = 3.141592653589793f;
			constexpr float TwoPi = 2.0f * Pi;
			if (Phase < 0.0f) { Phase += TwoPi; }
			if (Phase >= TwoPi) { Phase -= TwoPi; }
			const float Norm = Phase / TwoPi;  // [0, 1)
			switch (Shape)
			{
				case 0: return std::sin(Phase);                            // Sine
				case 1: return (Norm < 0.5f) ? (4.0f * Norm - 1.0f)        // Triangle
				                             : (3.0f - 4.0f * Norm);
				case 2: return (Norm < 0.5f) ? 1.0f : -1.0f;               // Square
				case 3: return 2.0f * Norm - 1.0f;                         // Saw
				default: return 0.0f;
			}
		}

		std::atomic<float>   Rate{ 4.0f };
		std::atomic<float>   Depth{ 0.5f };
		std::atomic<uint8_t> ShapeIdx{ 0 };
		std::atomic<uint8_t> StereoIdx{ 0 };

		double SampleRate = 48000.0;
		double LfoPhase = 0.0;
		FOnePoleSmoother RateSmoother;
		FOnePoleSmoother DepthSmoother;
	};
}
