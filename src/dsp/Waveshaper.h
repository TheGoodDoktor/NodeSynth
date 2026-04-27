#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	enum class EWaveshape : uint8_t
	{
		TanhSoft,
		HardClip,
		SoftClip,
		Fold,
		COUNT,
	};

	// Memoryless distortion. Drive applied pre-shape, Output applied post.
	// Both in dB so a slider sweep covers a useful range. Stateless aside from
	// the Drive / Output smoothers; no buffer allocation in Process.
	//
	// Aliasing note: at high Drive the shaped harmonics extend past Nyquist
	// and fold back. Oversampling is a Phase 5 deliverable; until then, drive
	// hard at your own ear's risk.
	class FWaveshaper : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Shape,
			Param_DriveDb,
			Param_OutputDb,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Waveshaper"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to distort." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio, "Shaped audio." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Shape",  0.0f, static_cast<float>(EWaveshape::COUNT) - 1.0f, 0.0f, false,
					EParamKind::Choice, { "TanhSoft", "HardClip", "SoftClip", "Fold" },
					"Distortion curve. TanhSoft is smooth saturation; HardClip is a sharp\n"
					"cutoff at ±1; SoftClip is a cubic soft-clipper; Fold reflects\n"
					"the signal back when it exceeds ±1." },
				{ "Drive",  0.0f, 40.0f, 0.0f, false, EParamKind::Float, {},
					"Pre-gain in dB. 0 = no boost; 6 ≈ ×2; 20 = ×10. Higher Drive pushes\n"
					"more of the signal into the nonlinear region. Smoothed." },
				{ "Output", -20.0f, 20.0f, 0.0f, false, EParamKind::Float, {},
					"Post-gain in dB. Compensates for the level boost / cut from heavy\n"
					"distortion. Smoothed." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Shape:    return static_cast<float>(Shape.load(std::memory_order_relaxed));
				case Param_DriveDb:  return DriveDb.load(std::memory_order_relaxed);
				case Param_OutputDb: return OutputDb.load(std::memory_order_relaxed);
				default:             return 0.0f;
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
					if (V >= static_cast<int32_t>(EWaveshape::COUNT))
					{
						V = static_cast<int32_t>(EWaveshape::COUNT) - 1;
					}
					Shape.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				case Param_DriveDb:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 40.0f) { V = 40.0f; }
					DriveDb.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_OutputDb:
				{
					float V = Value;
					if (V < -20.0f) { V = -20.0f; }
					if (V > 20.0f) { V = 20.0f; }
					OutputDb.store(V, std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			DriveSmoother.Prepare(InSampleRate);
			DriveSmoother.Reset(DbToLinear(DriveDb.load(std::memory_order_relaxed)));
			OutputSmoother.Prepare(InSampleRate);
			OutputSmoother.Reset(DbToLinear(OutputDb.load(std::memory_order_relaxed)));
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* AudioIn = GetInputBuffer(0);

			const EWaveshape ShapeNow = static_cast<EWaveshape>(Shape.load(std::memory_order_relaxed));
			DriveSmoother.SetTarget(DbToLinear(DriveDb.load(std::memory_order_relaxed)));
			OutputSmoother.SetTarget(DbToLinear(OutputDb.load(std::memory_order_relaxed)));

			if (AudioIn == nullptr)
			{
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					DriveSmoother.Tick();
					OutputSmoother.Tick();
					Out[I] = 0.0f;
				}
				return;
			}

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float Drive = DriveSmoother.Tick();
				const float OutGain = OutputSmoother.Tick();
				const float Driven = AudioIn[I] * Drive;
				Out[I] = ApplyShape(Driven, ShapeNow) * OutGain;
			}
		}

	private:
		static float DbToLinear(float Db)
		{
			return std::pow(10.0f, Db / 20.0f);
		}

		static float ApplyShape(float X, EWaveshape S)
		{
			switch (S)
			{
				case EWaveshape::TanhSoft:
					return std::tanh(X);
				case EWaveshape::HardClip:
					if (X > 1.0f) { return 1.0f; }
					if (X < -1.0f) { return -1.0f; }
					return X;
				case EWaveshape::SoftClip:
				{
					// Cubic soft clipper: 1.5x - 0.5x³ inside [-1, 1], saturate
					// at ±1 outside. Continuous derivative at the boundary so
					// it sounds smoother than HardClip.
					float Z = X;
					if (Z > 1.0f) { return 1.0f; }
					if (Z < -1.0f) { return -1.0f; }
					return 1.5f * Z - 0.5f * Z * Z * Z;
				}
				case EWaveshape::Fold:
				{
					// Triangular wave fold: bounce off ±1. The combined while
					// catches the case where one fold sends the value past the
					// other bound (e.g. 7.5 → -5.5 → 3.5 → -1.5 → -0.5).
					float Y = X;
					while (Y > 1.0f || Y < -1.0f)
					{
						if (Y > 1.0f) { Y = 2.0f - Y; }
						if (Y < -1.0f) { Y = -2.0f - Y; }
					}
					return Y;
				}
				default:
					return X;
			}
		}

		std::atomic<uint8_t> Shape{ static_cast<uint8_t>(EWaveshape::TanhSoft) };
		std::atomic<float>   DriveDb{ 0.0f };
		std::atomic<float>   OutputDb{ 0.0f };

		FOnePoleSmoother DriveSmoother;
		FOnePoleSmoother OutputSmoother;
	};
}
