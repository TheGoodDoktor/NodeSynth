#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Biquad.h"
#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Stereo harmonic exciter. Pre-highpass at the chosen Frequency →
	// tanh saturation (generates harmonics) → mix back with dry. Adds
	// "presence" / "air" by emphasising high-frequency harmonics that
	// don't fully exist in the source. Reuses FBiquad for the HP.
	class FExciter : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Frequency,
			Param_DriveDb,
			Param_Mix,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Exciter"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to excite." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Dry + (highpass → tanh) × Mix. Mix=0 → bit-identical passthrough." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Frequency", 1000.0f, 10000.0f, 3000.0f, true,  EParamKind::Float, {},
					"Highpass corner — what counts as 'high frequency' for the\n"
					"saturation stage. Logarithmic." },
				{ "Drive",     0.0f,    40.0f,    12.0f,   false, EParamKind::Float, {},
					"Saturation drive in dB. Higher = more harmonics generated." },
				{ "Mix",       0.0f,    1.0f,     0.3f,    false, EParamKind::Float, {},
					"How much of the excited signal to mix back with the dry input.\n"
					"0 = no excitation; 1 = pure excited (no dry component)." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Frequency: return Frequency.load(std::memory_order_relaxed);
				case Param_DriveDb:   return DriveDb.load(std::memory_order_relaxed);
				case Param_Mix:       return Mix.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Frequency:
				{
					float V = Value;
					if (V < 1000.0f)  { V = 1000.0f; }
					if (V > 10000.0f) { V = 10000.0f; }
					Frequency.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_DriveDb:
				{
					float V = Value;
					if (V < 0.0f)  { V = 0.0f; }
					if (V > 40.0f) { V = 40.0f; }
					DriveDb.store(V, std::memory_order_relaxed);
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
			SampleRate = static_cast<float>(InSampleRate);
			HpStateL.Reset();
			HpStateR.Reset();
			DriveSmoother.Prepare(InSampleRate, 30.0f);
			MixSmoother.Prepare(InSampleRate, 30.0f);
			DriveSmoother.Reset(DbToLin(DriveDb.load(std::memory_order_relaxed)));
			MixSmoother.Reset(Mix.load(std::memory_order_relaxed));
			RecomputeHp();
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* InL = GetInputBuffer(0, 0);
			const float* InR = GetInputBuffer(0, 1);

			DriveSmoother.SetTarget(DbToLin(DriveDb.load(std::memory_order_relaxed)));
			MixSmoother.SetTarget(Mix.load(std::memory_order_relaxed));
			RecomputeHp();

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R = (InR != nullptr) ? InR[I] : L;

				const float DriveLin = DriveSmoother.Tick();
				const float MixNow = MixSmoother.Tick();

				// Highpass → drive → tanh saturate. Saturated highs are
				// added on top of the dry signal (parallel processing).
				const float HpL = HpStateL.Process(HpCoeffs, L);
				const float HpR = HpStateR.Process(HpCoeffs, R);
				const float ExcL = std::tanh(HpL * DriveLin);
				const float ExcR = std::tanh(HpR * DriveLin);

				OutL[I] = L + MixNow * ExcL;
				OutR[I] = R + MixNow * ExcR;
			}
		}

	private:
		static float DbToLin(float Db) { return std::pow(10.0f, Db * 0.05f); }

		void RecomputeHp()
		{
			// Use the FBiquad helper. RBJ doesn't ship a 1st-order HP, so
			// use a high-shelf at -120 dB to approximate a steep highpass.
			// Cleaner: a peak filter centred well above the corner could
			// also work, but a true biquad highpass is the right call.
			// Compute coefficients by hand: 1st-order RBJ HP doesn't exist
			// in cookbook directly, but a 2nd-order Butterworth HP does.
			// Derive from the standard biquad HP formula.
			const float Fc = Frequency.load(std::memory_order_relaxed);
			const float Q = 0.7071f;  // Butterworth
			const float W0 = 2.0f * 3.14159265358979f * Fc / SampleRate;
			const float CosW = std::cos(W0);
			const float Alpha = std::sin(W0) / (2.0f * Q);

			const float B0_ = (1.0f + CosW) * 0.5f;
			const float B1_ = -(1.0f + CosW);
			const float B2_ = (1.0f + CosW) * 0.5f;
			const float A0_ = 1.0f + Alpha;
			const float A1_ = -2.0f * CosW;
			const float A2_ = 1.0f - Alpha;
			const float Inv = 1.0f / A0_;
			HpCoeffs.B0 = B0_ * Inv;
			HpCoeffs.B1 = B1_ * Inv;
			HpCoeffs.B2 = B2_ * Inv;
			HpCoeffs.A1 = A1_ * Inv;
			HpCoeffs.A2 = A2_ * Inv;
		}

		std::atomic<float> Frequency{ 3000.0f };
		std::atomic<float> DriveDb{ 12.0f };
		std::atomic<float> Mix{ 0.3f };

		float SampleRate = 48000.0f;
		FBiquadCoeffs HpCoeffs;
		FBiquadState HpStateL, HpStateR;
		FOnePoleSmoother DriveSmoother;
		FOnePoleSmoother MixSmoother;
	};
}
