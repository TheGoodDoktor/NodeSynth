#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	enum class ELfoShape : uint8_t
	{
		Sine,
		Triangle,
		Saw,
		Square,
		COUNT,
	};

	// Bipolar low-frequency oscillator. Free-running phase accumulator; rising
	// edge on Sync resets the phase to 0. Output is in [-Amount, +Amount];
	// chain a Scale node to remap to a unipolar range.
	//
	// Rate is one-pole smoothed so slider drags don't produce discontinuities;
	// other params are read raw at block start.
	class FLfo : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Shape,
			Param_RateHz,
			Param_Amount,
			Param_COUNT,
		};

		enum EInput : uint32_t
		{
			Input_Sync,
		};

		const char* GetTypeName() const override { return "LFO"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Sync", EPortType::Control } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Control } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Shape",  0.0f, static_cast<float>(ELfoShape::COUNT) - 1.0f, 0.0f, false,
					EParamKind::Choice, { "Sine", "Triangle", "Saw", "Square" },
					"LFO waveform." },
				{ "Rate",   0.01f, 50.0f, 1.0f, true,  EParamKind::Float, {},
					"Frequency in Hz. Slider is logarithmic. Smoothed." },
				{ "Amount", 0.0f,  1.0f,  1.0f, false, EParamKind::Float, {},
					"Output scaling (0..1). Multiplied with the raw shape value." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Shape:  return static_cast<float>(Shape.load(std::memory_order_relaxed));
				case Param_RateHz: return RateHz.load(std::memory_order_relaxed);
				case Param_Amount: return Amount.load(std::memory_order_relaxed);
				default:           return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Shape:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0) { V = 0; }
					if (V >= static_cast<int32_t>(ELfoShape::COUNT))
					{
						V = static_cast<int32_t>(ELfoShape::COUNT) - 1;
					}
					Shape.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				case Param_RateHz:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					RateHz.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Amount:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 1.0f) { V = 1.0f; }
					Amount.store(V, std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			Phase = 0.0f;
			bPrevSyncHigh = false;
			RateSmoother.Prepare(InSampleRate);
			RateSmoother.Reset(RateHz.load(std::memory_order_relaxed));
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* Sync = GetInputBuffer(Input_Sync);

			const ELfoShape ShapeNow = static_cast<ELfoShape>(Shape.load(std::memory_order_relaxed));
			const float AmountNow = Amount.load(std::memory_order_relaxed);
			RateSmoother.SetTarget(RateHz.load(std::memory_order_relaxed));

			const float InvSampleRate = static_cast<float>(1.0 / SampleRate);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float SyncV = (Sync != nullptr) ? Sync[I] : 0.0f;
				const bool bSyncHigh = SyncV > 0.5f;
				if (bSyncHigh && !bPrevSyncHigh)
				{
					Phase = 0.0f;
				}
				bPrevSyncHigh = bSyncHigh;

				const float SmoothedRate = RateSmoother.Tick();
				Phase += SmoothedRate * InvSampleRate;
				while (Phase >= 1.0f)
				{
					Phase -= 1.0f;
				}
				while (Phase < 0.0f)
				{
					Phase += 1.0f;
				}

				float Sample = 0.0f;
				switch (ShapeNow)
				{
					case ELfoShape::Sine:
						Sample = std::sin(Phase * 2.0f * 3.14159265358979323846f);
						break;
					case ELfoShape::Triangle:
						// Peak at phase 0, trough at 0.5: 4 * |phase - 0.5| - 1.
						Sample = 4.0f * std::fabs(Phase - 0.5f) - 1.0f;
						break;
					case ELfoShape::Saw:
						// Linear ramp from -1 at phase 0 to ~+1 at phase ~1.
						Sample = 2.0f * Phase - 1.0f;
						break;
					case ELfoShape::Square:
						Sample = (Phase < 0.5f) ? 1.0f : -1.0f;
						break;
					default:
						Sample = 0.0f;
						break;
				}

				Out[I] = Sample * AmountNow;
			}
		}

	private:
		std::atomic<uint8_t> Shape{ static_cast<uint8_t>(ELfoShape::Sine) };
		std::atomic<float>   RateHz{ 1.0f };
		std::atomic<float>   Amount{ 1.0f };

		double SampleRate = 48000.0;
		float Phase = 0.0f;
		bool bPrevSyncHigh = false;
		FOnePoleSmoother RateSmoother;
	};
}
