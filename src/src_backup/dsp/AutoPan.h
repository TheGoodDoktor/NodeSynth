#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Stereo auto-pan: LFO modulating L/R balance using constant-power pan
	// (cos / sin curves). Mono input becomes panned-stereo output, sweeping
	// between full-L and full-R with the LFO. Constant-power preserves
	// L² + R² ≈ Input² across the sweep — no loudness pumping at the centre.
	class FAutoPan : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Rate,
			Param_Depth,
			Param_Shape,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "AutoPan"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to pan." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Panned stereo. Constant-power: L² + R² ≈ Input² always." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Rate",  0.05f, 20.0f, 1.0f, true,  EParamKind::Float, {},
					"LFO rate in Hz. 1 Hz = slow sweep; >5 Hz = audibly fluttery.\n"
					"Logarithmic." },
				{ "Depth", 0.0f, 1.0f,   0.7f, false, EParamKind::Float, {},
					"Pan amount. 0 = always centred; 1 = full L↔R sweep." },
				{ "Shape", 0.0f, 3.0f,   0.0f, false, EParamKind::Choice,
					{ "Sine", "Triangle", "Square", "Saw" },
					"LFO waveform driving the pan position." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Rate:  return Rate.load(std::memory_order_relaxed);
				case Param_Depth: return Depth.load(std::memory_order_relaxed);
				case Param_Shape: return static_cast<float>(ShapeIdx.load(std::memory_order_relaxed));
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

			constexpr double TwoPi = 2.0 * 3.141592653589793;
			constexpr float Pi = 3.141592653589793f;
			constexpr float QuarterPi = Pi * 0.25f;

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float RateNow = RateSmoother.Tick();
				const float DepthNow = DepthSmoother.Tick();

				const float Lfo = LfoSample(Shape, static_cast<float>(LfoPhase));
				// Pan position in [-1, +1] scaled by Depth.
				const float Pan = DepthNow * Lfo;
				// Constant-power pan: angle θ = (Pan + 1) * π/4.
				// At Pan = -1 (full L): θ = 0     → cos=1, sin=0.
				// At Pan =  0 (centre): θ = π/4   → cos=sin=0.707.
				// At Pan = +1 (full R): θ = π/2   → cos=0, sin=1.
				const float Theta = (Pan + 1.0f) * QuarterPi;
				const float GainL = std::cos(Theta);
				const float GainR = std::sin(Theta);

				// Sum L+R of the input down to mono before re-panning. Stereo
				// content gets summed; mono input passes through unchanged.
				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R = (InR != nullptr) ? InR[I] : L;
				const float Mono = (L + R) * 0.5f;
				OutL[I] = Mono * GainL;
				OutR[I] = Mono * GainR;

				LfoPhase += TwoPi * RateNow / SampleRate;
				if (LfoPhase >= TwoPi) { LfoPhase -= TwoPi; }
			}
		}

	private:
		static float LfoSample(uint8_t Shape, float Phase)
		{
			constexpr float Pi = 3.141592653589793f;
			constexpr float TwoPi = 2.0f * Pi;
			if (Phase < 0.0f) { Phase += TwoPi; }
			if (Phase >= TwoPi) { Phase -= TwoPi; }
			const float Norm = Phase / TwoPi;
			switch (Shape)
			{
				case 0: return std::sin(Phase);
				case 1: return (Norm < 0.5f) ? (4.0f * Norm - 1.0f) : (3.0f - 4.0f * Norm);
				case 2: return (Norm < 0.5f) ? 1.0f : -1.0f;
				case 3: return 2.0f * Norm - 1.0f;
				default: return 0.0f;
			}
		}

		std::atomic<float>   Rate{ 1.0f };
		std::atomic<float>   Depth{ 0.7f };
		std::atomic<uint8_t> ShapeIdx{ 0 };

		double SampleRate = 48000.0;
		double LfoPhase = 0.0;
		FOnePoleSmoother RateSmoother;
		FOnePoleSmoother DepthSmoother;
	};
}
