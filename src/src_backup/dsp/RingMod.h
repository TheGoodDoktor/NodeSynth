#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Stereo ring modulator: input × internal carrier oscillator. Produces
	// sum + difference frequencies (input ± carrier), characteristic
	// metallic / bell timbre. Carrier shape: Sine / Triangle / Square.
	class FRingMod : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_CarrierHz,
			Param_Shape,
			Param_Mix,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "RingMod"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to modulate." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Input × carrier (internal oscillator). Mix 0 = dry passthrough." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Carrier", 1.0f, 5000.0f, 200.0f, true,  EParamKind::Float, {},
					"Carrier oscillator frequency in Hz. Logarithmic." },
				{ "Shape",   0.0f, 2.0f,    0.0f,   false, EParamKind::Choice,
					{ "Sine", "Triangle", "Square" },
					"Carrier waveform. Square is the harshest." },
				{ "Mix",     0.0f, 1.0f,    1.0f,   false, EParamKind::Float, {},
					"Wet/dry blend. 0 = dry, 1 = pure ring-modulated." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_CarrierHz: return CarrierHz.load(std::memory_order_relaxed);
				case Param_Shape:     return static_cast<float>(ShapeIdx.load(std::memory_order_relaxed));
				case Param_Mix:       return Mix.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_CarrierHz:
				{
					float V = Value;
					if (V < 1.0f)    { V = 1.0f; }
					if (V > 5000.0f) { V = 5000.0f; }
					CarrierHz.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Shape:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0) { V = 0; }
					if (V > 2) { V = 2; }
					ShapeIdx.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
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
			Phase = 0.0;
			CarrierSmoother.Prepare(InSampleRate, 30.0f);
			MixSmoother.Prepare(InSampleRate, 30.0f);
			CarrierSmoother.Reset(CarrierHz.load(std::memory_order_relaxed));
			MixSmoother.Reset(Mix.load(std::memory_order_relaxed));
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* InL = GetInputBuffer(0, 0);
			const float* InR = GetInputBuffer(0, 1);

			CarrierSmoother.SetTarget(CarrierHz.load(std::memory_order_relaxed));
			MixSmoother.SetTarget(Mix.load(std::memory_order_relaxed));
			const uint8_t Shape = ShapeIdx.load(std::memory_order_relaxed);

			constexpr double TwoPi = 2.0 * 3.141592653589793;

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float CarrierFreq = CarrierSmoother.Tick();
				const float MixNow = MixSmoother.Tick();

				const float Carrier = CarrierSample(Shape, static_cast<float>(Phase));

				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R = (InR != nullptr) ? InR[I] : L;
				const float WetL = L * Carrier;
				const float WetR = R * Carrier;

				OutL[I] = (1.0f - MixNow) * L + MixNow * WetL;
				OutR[I] = (1.0f - MixNow) * R + MixNow * WetR;

				Phase += TwoPi * CarrierFreq / SampleRate;
				if (Phase >= TwoPi) { Phase -= TwoPi; }
			}
		}

	private:
		static float CarrierSample(uint8_t Shape, float Ph)
		{
			constexpr float TwoPi = 2.0f * 3.141592653589793f;
			if (Ph < 0.0f) { Ph += TwoPi; }
			if (Ph >= TwoPi) { Ph -= TwoPi; }
			const float Norm = Ph / TwoPi;
			switch (Shape)
			{
				case 0: return std::sin(Ph);                                       // Sine
				case 1: return (Norm < 0.5f) ? (4.0f * Norm - 1.0f) : (3.0f - 4.0f * Norm);  // Triangle
				case 2: return (Norm < 0.5f) ? 1.0f : -1.0f;                       // Square
				default: return 0.0f;
			}
		}

		std::atomic<float>   CarrierHz{ 200.0f };
		std::atomic<uint8_t> ShapeIdx{ 0 };
		std::atomic<float>   Mix{ 1.0f };

		double SampleRate = 48000.0;
		double Phase = 0.0;
		FOnePoleSmoother CarrierSmoother;
		FOnePoleSmoother MixSmoother;
	};
}
