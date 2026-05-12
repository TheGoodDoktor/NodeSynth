#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Biquad.h"
#include "dsp/Node.h"

namespace NodeSynth
{
	// Stereo 3-band EQ: low-shelf + peak + high-shelf in series. Standard
	// musical-EQ topology covering tilt + midrange surgery in seven params.
	// Coefficients computed once per block, applied via two FBiquadState
	// pairs (one per channel). All gains at 0 dB → bit-identical passthrough.
	class FEqualizer : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_LowShelfFreq,
			Param_LowShelfGainDb,
			Param_PeakFreq,
			Param_PeakGainDb,
			Param_PeakQ,
			Param_HighShelfFreq,
			Param_HighShelfGainDb,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Equalizer"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to equalise." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Equalised audio. All three gains at 0 dB → bit-identical\n"
				"passthrough (within float arithmetic tolerance)." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "LowFreq",  20.0f,   1000.0f, 200.0f,  true,  EParamKind::Float, {},
					"Low-shelf corner frequency in Hz. Logarithmic." },
				{ "LowGain",  -18.0f,   18.0f,   0.0f,  false, EParamKind::Float, {},
					"Low-shelf gain in dB. Boosts or cuts everything below LowFreq." },
				{ "MidFreq",  100.0f, 10000.0f, 1000.0f, true,  EParamKind::Float, {},
					"Mid-band peak frequency in Hz. Logarithmic." },
				{ "MidGain",  -18.0f,   18.0f,   0.0f,  false, EParamKind::Float, {},
					"Mid-band peak gain in dB. Boosts or cuts a bell around MidFreq." },
				{ "MidQ",      0.3f,   10.0f,    1.0f,  true,  EParamKind::Float, {},
					"Mid-band Q (resonance / width). Higher = narrower bell." },
				{ "HighFreq", 1000.0f, 18000.0f, 8000.0f, true,  EParamKind::Float, {},
					"High-shelf corner frequency in Hz. Logarithmic." },
				{ "HighGain", -18.0f,   18.0f,   0.0f,  false, EParamKind::Float, {},
					"High-shelf gain in dB. Boosts or cuts everything above HighFreq." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_LowShelfFreq:    return LowFreq.load(std::memory_order_relaxed);
				case Param_LowShelfGainDb:  return LowGainDb.load(std::memory_order_relaxed);
				case Param_PeakFreq:        return MidFreq.load(std::memory_order_relaxed);
				case Param_PeakGainDb:      return MidGainDb.load(std::memory_order_relaxed);
				case Param_PeakQ:           return MidQ.load(std::memory_order_relaxed);
				case Param_HighShelfFreq:   return HighFreq.load(std::memory_order_relaxed);
				case Param_HighShelfGainDb: return HighGainDb.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_LowShelfFreq:    LowFreq.store(Clamp(Value, 20.0f, 1000.0f), std::memory_order_relaxed); break;
				case Param_LowShelfGainDb:  LowGainDb.store(Clamp(Value, -18.0f, 18.0f), std::memory_order_relaxed); break;
				case Param_PeakFreq:        MidFreq.store(Clamp(Value, 100.0f, 10000.0f), std::memory_order_relaxed); break;
				case Param_PeakGainDb:      MidGainDb.store(Clamp(Value, -18.0f, 18.0f), std::memory_order_relaxed); break;
				case Param_PeakQ:           MidQ.store(Clamp(Value, 0.3f, 10.0f), std::memory_order_relaxed); break;
				case Param_HighShelfFreq:   HighFreq.store(Clamp(Value, 1000.0f, 18000.0f), std::memory_order_relaxed); break;
				case Param_HighShelfGainDb: HighGainDb.store(Clamp(Value, -18.0f, 18.0f), std::memory_order_relaxed); break;
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = static_cast<float>(InSampleRate);
			for (FBiquadState& S : LowState)  { S.Reset(); }
			for (FBiquadState& S : MidState)  { S.Reset(); }
			for (FBiquadState& S : HighState) { S.Reset(); }
			RecomputeCoeffs();
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* InL = GetInputBuffer(0, 0);
			const float* InR = GetInputBuffer(0, 1);

			RecomputeCoeffs();

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R = (InR != nullptr) ? InR[I] : L;

				float YL = LowState[0].Process(LowCoeffs, L);
				YL = MidState[0].Process(MidCoeffs, YL);
				YL = HighState[0].Process(HighCoeffs, YL);

				float YR = LowState[1].Process(LowCoeffs, R);
				YR = MidState[1].Process(MidCoeffs, YR);
				YR = HighState[1].Process(HighCoeffs, YR);

				OutL[I] = YL;
				OutR[I] = YR;
			}
		}

	private:
		static float Clamp(float V, float Min, float Max)
		{
			if (V < Min) { return Min; }
			if (V > Max) { return Max; }
			return V;
		}

		void RecomputeCoeffs()
		{
			// Default Q for shelves — the RBJ shelf formula uses Q to control
			// the slope. 0.7071 (= 1/sqrt(2)) gives a Butterworth-style
			// monotonic shelf without any pre/post-corner ripple.
			constexpr float ShelfQ = 0.7071f;
			LowCoeffs.SetLowShelf(LowFreq.load(std::memory_order_relaxed), ShelfQ,
				LowGainDb.load(std::memory_order_relaxed), SampleRate);
			MidCoeffs.SetPeak(MidFreq.load(std::memory_order_relaxed),
				MidQ.load(std::memory_order_relaxed),
				MidGainDb.load(std::memory_order_relaxed), SampleRate);
			HighCoeffs.SetHighShelf(HighFreq.load(std::memory_order_relaxed), ShelfQ,
				HighGainDb.load(std::memory_order_relaxed), SampleRate);
		}

		std::atomic<float> LowFreq{ 200.0f };
		std::atomic<float> LowGainDb{ 0.0f };
		std::atomic<float> MidFreq{ 1000.0f };
		std::atomic<float> MidGainDb{ 0.0f };
		std::atomic<float> MidQ{ 1.0f };
		std::atomic<float> HighFreq{ 8000.0f };
		std::atomic<float> HighGainDb{ 0.0f };

		float SampleRate = 48000.0f;
		FBiquadCoeffs LowCoeffs, MidCoeffs, HighCoeffs;
		FBiquadState LowState[2], MidState[2], HighState[2];
	};
}
