#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	enum class EOscShape : uint32_t
	{
		Sine = 0,
		Saw,
		Square,
		Triangle,
		Noise,
		COUNT,
	};

	class FOscillator : public TNodeBase<2, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Shape,
			Param_Frequency,
			Param_Amplitude,
			Param_COUNT,
		};

		enum EInput : uint32_t
		{
			Input_Frequency,
			Input_Amplitude,
		};

		FOscillator()
		{
			// Lightweight splitmix to turn any address into a non-zero 32-bit seed.
			uint64_t Z = reinterpret_cast<uintptr_t>(this) + 0x9E3779B97F4A7C15ULL;
			Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ULL;
			Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBULL;
			NoiseState = static_cast<uint32_t>(Z ^ (Z >> 31));
			if (NoiseState == 0)
			{
				NoiseState = 0xA1B2C3D4;
			}
		}

		const char* GetTypeName() const override { return "Oscillator"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "Freq", EPortType::Control,
					"Frequency in Hz. Overrides the Frequency param when connected." },
				{ "Amp",  EPortType::Control,
					"Amplitude (0..1). Overrides the Amplitude param when connected. Smoothed." },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Audio output. Waveform shape comes from the Shape param." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Shape", 0.0f, static_cast<float>(EOscShape::COUNT) - 1, 0.0f, false, EParamKind::Choice,
					{ "Sine", "Saw", "Square", "Triangle", "Noise" },
					"Waveform. Saw / Square / Triangle use PolyBLEP to suppress aliasing; Noise is white per-sample." },
				{ "Frequency", 20.0f, 20000.0f, 440.0f, true,  EParamKind::Float, {},
					"Pitch in Hz. The Freq Control input overrides this when connected.",
					/* bHidden */ false, /* ControlInputIndex */ Input_Frequency },
				{ "Amplitude", 0.0f,  1.0f,     0.3f,  false, EParamKind::Float, {},
					"Output level (0..1). The Amp Control input overrides this when connected. Smoothed.",
					/* bHidden */ false, /* ControlInputIndex */ Input_Amplitude },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Shape:     return static_cast<float>(Shape.load(std::memory_order_relaxed));
				case Param_Frequency: return Frequency.load(std::memory_order_relaxed);
				case Param_Amplitude: return Amplitude.load(std::memory_order_relaxed);
				default:              return 0.0f;
			}
		}

		float GetLiveParamValue(uint32_t Index) const override
		{
			if (Index != Param_Frequency && Index != Param_Amplitude)
			{
				return GetParamValue(Index);
			}
			// Scan per-voice slots for the loudest voice — that's the one
			// the user is hearing most prominently. Mono master keeps slot 0
			// and the others stay at zero, so the scan also works there.
			int32_t Best = 0;
			float BestAmp = LastAmplitudePerVoice[0].load(std::memory_order_relaxed);
			for (int32_t V = 1; V < LiveMaxVoices; ++V)
			{
				const float A = LastAmplitudePerVoice[V].load(std::memory_order_relaxed);
				if (A > BestAmp) { BestAmp = A; Best = V; }
			}
			if (Index == Param_Amplitude) { return BestAmp; }
			// Frequency: when the loudest voice is silent, fall back to the
			// param value so the slider doesn't show stale freq from a long-
			// dead note.
			if (BestAmp <= 1e-3f) { return Frequency.load(std::memory_order_relaxed); }
			return LastFrequencyPerVoice[Best].load(std::memory_order_relaxed);
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Shape:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0)
					{
						V = 0;
					}
					if (V >= static_cast<int32_t>(EOscShape::COUNT))
					{
						V = static_cast<int32_t>(EOscShape::COUNT) - 1;
					}
					Shape.store(static_cast<uint32_t>(V), std::memory_order_relaxed);
					break;
				}
				case Param_Frequency: Frequency.store(Value, std::memory_order_relaxed); break;
				case Param_Amplitude: Amplitude.store(Value, std::memory_order_relaxed); break;
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			AmpSmoother.Prepare(InSampleRate);
			AmpSmoother.Reset(Amplitude.load(std::memory_order_relaxed));
			// Reset triangle-integrator on rate change to prevent startup DC transient.
			TriangleState = 0.0f;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* FreqCv = GetInputBuffer(Input_Frequency);
			const float* AmpCv = GetInputBuffer(Input_Amplitude);
			const EOscShape S = static_cast<EOscShape>(Shape.load(std::memory_order_relaxed));
			const float FreqParam = Frequency.load(std::memory_order_relaxed);
			AmpSmoother.SetTarget(Amplitude.load(std::memory_order_relaxed));

			auto FreqAt = [&](uint32_t I) -> double
			{
				// Frequency control buffer is in Hz. Clamp to [1, Nyquist*0.99] to stay stable.
				const float V = (FreqCv != nullptr) ? FreqCv[I] : FreqParam;
				const float Nyq = static_cast<float>(0.49 * SampleRate);
				if (V < 1.0f) { return 1.0; }
				if (V > Nyq) { return static_cast<double>(Nyq); }
				return static_cast<double>(V);
			};
			auto AmpAt = [&](uint32_t I) -> float
			{
				// When Amplitude is CV-driven we skip the smoother — the CV is already
				// sample-rate and we want direct modulation (tremolo, VCA-like use).
				return (AmpCv != nullptr) ? AmpCv[I] : AmpSmoother.Tick();
			};

			// Snapshots the effective Frequency / Amplitude at the last
			// sample of the block, written into the master's per-voice
			// arrays at our own VoiceIndex. The UI scans all voices for the
			// loudest one and reads from its slot — no race on a shared
			// atomic, no last-writer-wins instability.
			auto LatchLive = [&]()
			{
				const uint32_t Last = (Ctx.BlockSize > 0) ? Ctx.BlockSize - 1u : 0u;
				const float FreqLast = (FreqCv != nullptr) ? FreqCv[Last] : FreqParam;
				const float AmpLast = (AmpCv != nullptr) ? AmpCv[Last]
					: Amplitude.load(std::memory_order_relaxed);
				auto* Target = (MasterMirror != nullptr)
					? static_cast<FOscillator*>(MasterMirror) : this;
				const int32_t Slot = (VoiceIndex >= 0 && VoiceIndex < LiveMaxVoices)
					? VoiceIndex : 0;
				Target->LastFrequencyPerVoice[Slot].store(FreqLast, std::memory_order_relaxed);
				Target->LastAmplitudePerVoice[Slot].store(AmpLast, std::memory_order_relaxed);
			};

			// Sine keeps radians for readability; others run on normalised phase [0, 1).
			if (S == EOscShape::Sine)
			{
				const double TwoPi = std::numbers::pi * 2.0;
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					const double PhaseInc = TwoPi * FreqAt(I) / SampleRate;
					Out[I] = AmpAt(I) * static_cast<float>(std::sin(SinePhase));
					SinePhase += PhaseInc;
					if (SinePhase >= TwoPi)
					{
						SinePhase -= TwoPi;
					}
				}
				LatchLive();
				return;
			}

			if (S == EOscShape::Noise)
			{
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					uint32_t X = NoiseState;
					X ^= X << 13;
					X ^= X >> 17;
					X ^= X << 5;
					NoiseState = X;
					const float Sample = static_cast<float>(static_cast<int32_t>(X)) * (1.0f / 2147483648.0f);
					Out[I] = AmpAt(I) * Sample;
				}
				LatchLive();
				return;
			}

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const double Dt = FreqAt(I) / SampleRate;

				float Value = 0.0f;
				switch (S)
				{
					case EOscShape::Saw:
					{
						Value = static_cast<float>(2.0 * Phase - 1.0);
						Value -= static_cast<float>(PolyBlep(Phase, Dt));
						break;
					}
					case EOscShape::Square:
					{
						Value = (Phase < 0.5) ? 1.0f : -1.0f;
						Value += static_cast<float>(PolyBlep(Phase, Dt));
						Value -= static_cast<float>(PolyBlep(std::fmod(Phase + 0.5, 1.0), Dt));
						break;
					}
					case EOscShape::Triangle:
					{
						float SquareSample = (Phase < 0.5) ? 1.0f : -1.0f;
						SquareSample += static_cast<float>(PolyBlep(Phase, Dt));
						SquareSample -= static_cast<float>(PolyBlep(std::fmod(Phase + 0.5, 1.0), Dt));
						const float Integrator = static_cast<float>(4.0 * Dt);
						TriangleState = Integrator * SquareSample + (1.0f - Integrator) * TriangleState;
						Value = TriangleState;
						break;
					}
					default:
						break;
				}

				Out[I] = AmpAt(I) * Value;

				Phase += Dt;
				if (Phase >= 1.0)
				{
					Phase -= 1.0;
				}
			}
			LatchLive();
		}

	private:
		// Standard PolyBLEP correction for a rising discontinuity at phi == 0.
		// Returns the correction to subtract from a naive saw (or add/subtract around
		// the two edges of a naive square) to get an anti-aliased waveform.
		static double PolyBlep(double Phi, double Dt)
		{
			if (Phi < Dt)
			{
				const double T = Phi / Dt;
				return T + T - T * T - 1.0;
			}
			if (Phi > 1.0 - Dt)
			{
				const double T = (Phi - 1.0) / Dt;
				return T * T + T + T + 1.0;
			}
			return 0.0;
		}

		std::atomic<uint32_t> Shape{ static_cast<uint32_t>(EOscShape::Sine) };
		std::atomic<float> Frequency{ 440.0f };
		std::atomic<float> Amplitude{ 0.3f };
		// Per-voice last-sample Frequency / Amplitude. Each clone writes its
		// own VoiceIndex slot; the master's GetLiveParamValue scans for the
		// loudest voice and returns its values. Mono nodes use slot 0.
		std::atomic<float> LastFrequencyPerVoice[LiveMaxVoices] = {};
		std::atomic<float> LastAmplitudePerVoice[LiveMaxVoices] = {};
		FOnePoleSmoother AmpSmoother;
		double Phase = 0.0;         // normalised [0, 1) for Saw/Square/Triangle
		double SinePhase = 0.0;     // radians, Sine only
		double SampleRate = 48000.0;
		float TriangleState = 0.0f;
		uint32_t NoiseState = 1;
	};
}
