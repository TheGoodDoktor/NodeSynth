#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Biquad.h"
#include "dsp/Envelope.h"
#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Classic channel vocoder. The modulator (In 1, classically a voice) is
	// split into N log-spaced bands by an analysis filter bank; a per-band
	// envelope follower tracks each band's amplitude. The carrier (In 0,
	// classically a saw/square synth) is split into the SAME N bands by a
	// synthesis bank; each carrier band is scaled by its matching modulator
	// envelope, and the scaled bands are summed. Where the modulator has
	// energy in band i, the carrier passes in band i — so the carrier "wears"
	// the modulator's spectral shape ("talking synth").
	//
	// Bands are 4-pole (two cascaded RBJ band-pass biquads) for skirts steep
	// enough to keep speech intelligible. Envelope detection is mono (L+R sum
	// of the modulator); synthesis runs per-channel so a stereo carrier keeps
	// its image. No runtime allocation — every bank is a fixed MaxBands array.
	class FVocoder : public TNodeBase<2, 1>
	{
	public:
		static constexpr int32_t MaxBands = 24;

		enum EInput : uint32_t
		{
			Input_Carrier,
			Input_Modulator,
		};

		enum EParam : uint32_t
		{
			Param_Bands,
			Param_Attack,
			Param_Release,
			Param_Formant,
			Param_Mix,
			Param_OutputDb,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Vocoder"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "Carrier",   EPortType::Audio,
					"Harmonically-rich signal to be shaped — a saw / square\n"
					"oscillator works best. This is what you hear." },
				{ "Modulator", EPortType::Audio,
					"Signal whose moving spectrum is imposed on the carrier —\n"
					"classically a voice (wire a Mic Input here)." },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Carrier shaped by the modulator's spectral envelope." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Bands",   0.0f, 2.0f,   1.0f,  false, EParamKind::Choice,
					{ "8", "16", "24" },
					"Number of analysis / synthesis bands. More = more\n"
					"intelligible and smoother; fewer = coarser / more 'robot'." },
				{ "Attack",  0.5f, 50.0f,  5.0f,  true,  EParamKind::Float, {},
					"Envelope-follower attack in ms. Fast attack keeps\n"
					"consonants crisp." },
				{ "Release", 5.0f, 500.0f, 40.0f, true,  EParamKind::Float, {},
					"Envelope-follower release in ms. Long release smears the\n"
					"sound into a lush choir / pad." },
				{ "Formant", 0.5f, 2.0f,   1.0f,  false, EParamKind::Float, {},
					"Shifts the synthesis band centres relative to analysis.\n"
					"1 = true vocoder; >1 'chipmunk', <1 'monster'." },
				{ "Mix",     0.0f, 1.0f,   1.0f,  false, EParamKind::Float, {},
					"Dry/wet blend. 0 = dry carrier passthrough, 1 = fully\n"
					"vocoded." },
				{ "Output",  -24.0f, 24.0f, 0.0f, false, EParamKind::Float, {},
					"Makeup gain in dB. Vocoding loses level — boost it back." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Bands:    return static_cast<float>(BandsIdx.load(std::memory_order_relaxed));
				case Param_Attack:   return AttackMs.load(std::memory_order_relaxed);
				case Param_Release:  return ReleaseMs.load(std::memory_order_relaxed);
				case Param_Formant:  return Formant.load(std::memory_order_relaxed);
				case Param_Mix:      return Mix.load(std::memory_order_relaxed);
				case Param_OutputDb: return OutputDb.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Bands:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0) { V = 0; }
					if (V > 2) { V = 2; }
					BandsIdx.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				case Param_Attack:   AttackMs.store(Clamp(Value, 0.5f, 50.0f), std::memory_order_relaxed); break;
				case Param_Release:  ReleaseMs.store(Clamp(Value, 5.0f, 500.0f), std::memory_order_relaxed); break;
				case Param_Formant:  Formant.store(Clamp(Value, 0.5f, 2.0f), std::memory_order_relaxed); break;
				case Param_Mix:      Mix.store(Clamp(Value, 0.0f, 1.0f), std::memory_order_relaxed); break;
				case Param_OutputDb: OutputDb.store(Clamp(Value, -24.0f, 24.0f), std::memory_order_relaxed); break;
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = static_cast<float>(InSampleRate);

			for (int32_t B = 0; B < MaxBands; ++B)
			{
				Envelopes[B].Prepare(InSampleRate);
				AnalysisState[B][0].Reset();
				AnalysisState[B][1].Reset();
				SynthStateL[B][0].Reset();
				SynthStateL[B][1].Reset();
				SynthStateR[B][0].Reset();
				SynthStateR[B][1].Reset();
			}

			GainSmoother.Prepare(InSampleRate, 30.0f);
			GainSmoother.Reset(DbToLin(OutputDb.load(std::memory_order_relaxed)));

			// Force a full recompute on the first block.
			AppliedBandCount = 0;
			AppliedFormant = -1.0f;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);

			const float* CarL = GetInputBuffer(Input_Carrier, 0);
			const float* CarR = GetInputBuffer(Input_Carrier, 1);
			const float* ModL = GetInputBuffer(Input_Modulator, 0);
			const float* ModR = GetInputBuffer(Input_Modulator, 1);

			const int32_t N = BandCountFromIdx(BandsIdx.load(std::memory_order_relaxed));
			const float FormantNow = Formant.load(std::memory_order_relaxed);

			// Band count change retunes the whole bank and resets state (accept
			// one boundary block of discontinuity); a formant change retunes
			// only the synthesis bank.
			if (N != AppliedBandCount)
			{
				ComputeBands(N);
				RecomputeAnalysisCoeffs(N);
				RecomputeSynthCoeffs(N, FormantNow);
				for (int32_t B = 0; B < MaxBands; ++B)
				{
					AnalysisState[B][0].Reset();
					AnalysisState[B][1].Reset();
					SynthStateL[B][0].Reset();
					SynthStateL[B][1].Reset();
					SynthStateR[B][0].Reset();
					SynthStateR[B][1].Reset();
				}
				AppliedBandCount = N;
				AppliedFormant = FormantNow;
			}
			else if (FormantNow != AppliedFormant)
			{
				RecomputeSynthCoeffs(N, FormantNow);
				AppliedFormant = FormantNow;
			}

			for (int32_t B = 0; B < N; ++B)
			{
				Envelopes[B].SetTimes(AttackMs.load(std::memory_order_relaxed),
					ReleaseMs.load(std::memory_order_relaxed));
			}

			GainSmoother.SetTarget(DbToLin(OutputDb.load(std::memory_order_relaxed)));
			const float MixNow = Mix.load(std::memory_order_relaxed);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float CL = (CarL != nullptr) ? CarL[I] : 0.0f;
				const float CR = (CarR != nullptr) ? CarR[I] : CL;

				const float ML = (ModL != nullptr) ? ModL[I] : 0.0f;
				const float MR = (ModR != nullptr) ? ModR[I] : ML;
				const float ModMono = 0.5f * (ML + MR);

				float SumL = 0.0f;
				float SumR = 0.0f;

				for (int32_t B = 0; B < N; ++B)
				{
					// Analysis: band-limit the modulator, follow its envelope.
					float A = AnalysisState[B][0].Process(AnalysisCoeffs[B], ModMono);
					A = AnalysisState[B][1].Process(AnalysisCoeffs[B], A);
					const float EnvB = Envelopes[B].Process(A);

					// Synthesis: band-limit the carrier, scale by that envelope.
					float YL = SynthStateL[B][0].Process(SynthCoeffs[B], CL);
					YL = SynthStateL[B][1].Process(SynthCoeffs[B], YL);
					SumL += YL * EnvB;

					float YR = SynthStateR[B][0].Process(SynthCoeffs[B], CR);
					YR = SynthStateR[B][1].Process(SynthCoeffs[B], YR);
					SumR += YR * EnvB;
				}

				const float Gain = GainSmoother.Tick();
				const float WetL = SumL * Gain;
				const float WetR = SumR * Gain;

				OutL[I] = (1.0f - MixNow) * CL + MixNow * WetL;
				OutR[I] = (1.0f - MixNow) * CR + MixNow * WetR;
			}
		}

	private:
		static float Clamp(float V, float Min, float Max)
		{
			if (V < Min) { return Min; }
			if (V > Max) { return Max; }
			return V;
		}

		static float DbToLin(float Db)
		{
			return std::pow(10.0f, Db / 20.0f);
		}

		static int32_t BandCountFromIdx(uint8_t Idx)
		{
			switch (Idx)
			{
				case 0:  return 8;
				case 2:  return 24;
				default: return 16;
			}
		}

		// Fill Centers[0..N-1] with geometrically-spaced band centres and derive
		// the constant-Q used across all bands. The -3 dB points of adjacent
		// single-section bands meet at the neighbour's centre, so the bank tiles
		// the spectrum; cascading two sections narrows it (better separation).
		void ComputeBands(int32_t N)
		{
			const float FMin = 120.0f;
			const float FMax = (8000.0f < 0.45f * SampleRate) ? 8000.0f : 0.45f * SampleRate;
			if (N <= 1)
			{
				Centers[0] = 0.5f * (FMin + FMax);
				BandQ = 4.0f;
				return;
			}

			const float Ratio = std::pow(FMax / FMin, 1.0f / static_cast<float>(N - 1));
			const float SqrtR = std::sqrt(Ratio);
			BandQ = 1.0f / (SqrtR - 1.0f / SqrtR);
			for (int32_t B = 0; B < N; ++B)
			{
				Centers[B] = FMin * std::pow(Ratio, static_cast<float>(B));
			}
		}

		void RecomputeAnalysisCoeffs(int32_t N)
		{
			for (int32_t B = 0; B < N; ++B)
			{
				AnalysisCoeffs[B].SetBandpass(ClampFreq(Centers[B]), BandQ, SampleRate);
			}
		}

		void RecomputeSynthCoeffs(int32_t N, float FormantShift)
		{
			for (int32_t B = 0; B < N; ++B)
			{
				SynthCoeffs[B].SetBandpass(ClampFreq(Centers[B] * FormantShift), BandQ, SampleRate);
			}
		}

		float ClampFreq(float Fc) const
		{
			const float Hi = 0.45f * SampleRate;
			if (Fc < 20.0f) { return 20.0f; }
			if (Fc > Hi) { return Hi; }
			return Fc;
		}

		std::atomic<uint8_t> BandsIdx{ 1 };   // 16 bands
		std::atomic<float>   AttackMs{ 5.0f };
		std::atomic<float>   ReleaseMs{ 40.0f };
		std::atomic<float>   Formant{ 1.0f };
		std::atomic<float>   Mix{ 1.0f };
		std::atomic<float>   OutputDb{ 0.0f };

		float SampleRate = 48000.0f;
		float Centers[MaxBands] = {};
		float BandQ = 4.0f;

		// Recompute guards — last band count / formant actually applied to coeffs.
		int32_t AppliedBandCount = 0;
		float AppliedFormant = -1.0f;

		FBiquadCoeffs AnalysisCoeffs[MaxBands];
		FBiquadCoeffs SynthCoeffs[MaxBands];
		FBiquadState  AnalysisState[MaxBands][2];
		FBiquadState  SynthStateL[MaxBands][2];
		FBiquadState  SynthStateR[MaxBands][2];
		FEnvelopeFollower Envelopes[MaxBands];

		FOnePoleSmoother GainSmoother;
	};
}
