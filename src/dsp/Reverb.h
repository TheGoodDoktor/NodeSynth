#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Freeverb by Jezar at Dreampoint (public domain, c. 2000): 8 lowpass-
	// feedback comb filters in parallel summed into 4 series allpass diffusers.
	// Two parallel filter banks — L uses the published tunings; R uses the
	// same tunings + StereoSpread samples per the standard Freeverb stereo
	// recipe. Mono input fans out to both banks; the output is true stereo
	// (channel 0 = L bank, channel 1 = R bank).
	//
	// Standard Freeverb delay tunings are specified at 44100 Hz; we scale them
	// by SampleRate / 44100 to track the device. Comb and allpass buffers
	// allocate in Prepare; Process performs no allocation.
	class FReverb : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_RoomSize,
			Param_Damping,
			Param_Wet,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Reverb"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to reverberate." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Dry × (1-Wet) + reverberated × Wet, stereo. Mix is internal so\n"
				"the node can replace a dry → effect chain without a blender." } };
		}

		bool IsOutputStereo(uint32_t Index) const override
		{
			return Index == 0;
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "RoomSize", 0.0f, 1.0f, 0.5f, false, EParamKind::Float, {},
					"Comb feedback gain. Bigger room = longer tail." },
				{ "Damping",  0.0f, 1.0f, 0.5f, false, EParamKind::Float, {},
					"High-frequency damping in the comb feedback path. 0 = bright, 1 = very dark." },
				{ "Wet",      0.0f, 1.0f, 0.3f, false, EParamKind::Float, {},
					"Wet/dry mix. 0 = dry passthrough; 1 = pure reverb tail with no dry signal." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_RoomSize: return RoomSize.load(std::memory_order_relaxed);
				case Param_Damping:  return Damping.load(std::memory_order_relaxed);
				case Param_Wet:      return Wet.load(std::memory_order_relaxed);
				default:             return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			float V = Value;
			if (V < 0.0f) { V = 0.0f; }
			if (V > 1.0f) { V = 1.0f; }
			switch (Index)
			{
				case Param_RoomSize: RoomSize.store(V, std::memory_order_relaxed); break;
				case Param_Damping:  Damping.store(V, std::memory_order_relaxed); break;
				case Param_Wet:      Wet.store(V, std::memory_order_relaxed); break;
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			const double Scale = InSampleRate / 44100.0;
			const size_t Spread = static_cast<size_t>(StereoSpread * Scale);

			for (int32_t I = 0; I < NumCombs; ++I)
			{
				const size_t Base = static_cast<size_t>(CombTunings[I] * Scale);
				CombsL[I].Buffer.assign(Base, 0.0f);
				CombsL[I].Index = 0;
				CombsL[I].FilterStore = 0.0f;
				CombsR[I].Buffer.assign(Base + Spread, 0.0f);
				CombsR[I].Index = 0;
				CombsR[I].FilterStore = 0.0f;
			}
			for (int32_t I = 0; I < NumAllpasses; ++I)
			{
				const size_t Base = static_cast<size_t>(AllpassTunings[I] * Scale);
				AllpassesL[I].Buffer.assign(Base, 0.0f);
				AllpassesL[I].Index = 0;
				AllpassesR[I].Buffer.assign(Base + Spread, 0.0f);
				AllpassesR[I].Index = 0;
			}
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* AudioInL = GetInputBuffer(0, 0);
			const float* AudioInR = GetInputBuffer(0, 1);

			const float RoomNow = RoomSize.load(std::memory_order_relaxed);
			const float DampNow = Damping.load(std::memory_order_relaxed);
			const float WetNow = Wet.load(std::memory_order_relaxed);
			// Standard Freeverb mapping: feedback = 0.7 + 0.28 * RoomSize so
			// even RoomSize = 0 gives some tail and RoomSize = 1 stays just
			// short of self-oscillation.
			const float CombFeedback = 0.7f + 0.28f * RoomNow;
			const float CombDamping = DampNow;

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float DryL = (AudioInL != nullptr) ? AudioInL[I] : 0.0f;
				const float DryR = (AudioInR != nullptr) ? AudioInR[I] : DryL;
				// Standard Freeverb input scale — keeps the 8-comb sum within
				// reasonable bounds.
				const float ScaledL = DryL * 0.015f;
				const float ScaledR = DryR * 0.015f;

				float CombSumL = 0.0f;
				float CombSumR = 0.0f;
				for (int32_t C = 0; C < NumCombs; ++C)
				{
					CombSumL += CombsL[C].Process(ScaledL, CombFeedback, CombDamping);
					CombSumR += CombsR[C].Process(ScaledR, CombFeedback, CombDamping);
				}

				float DiffusedL = CombSumL;
				float DiffusedR = CombSumR;
				for (int32_t A = 0; A < NumAllpasses; ++A)
				{
					DiffusedL = AllpassesL[A].Process(DiffusedL);
					DiffusedR = AllpassesR[A].Process(DiffusedR);
				}

				OutL[I] = DryL * (1.0f - WetNow) + DiffusedL * WetNow;
				OutR[I] = DryR * (1.0f - WetNow) + DiffusedR * WetNow;
			}
		}

	private:
		// One lowpass-feedback comb. Original Freeverb paper's recipe.
		struct FComb
		{
			std::vector<float> Buffer;
			size_t Index = 0;
			float FilterStore = 0.0f;

			float Process(float Input, float Feedback, float Damping)
			{
				const float Output = Buffer[Index];
				// One-pole LP across the feedback tap.
				FilterStore = Output * (1.0f - Damping) + FilterStore * Damping;
				Buffer[Index] = Input + FilterStore * Feedback;
				++Index;
				if (Index >= Buffer.size())
				{
					Index = 0;
				}
				return Output;
			}
		};

		struct FAllpass
		{
			std::vector<float> Buffer;
			size_t Index = 0;

			float Process(float Input)
			{
				constexpr float Feedback = 0.5f;
				const float BufOut = Buffer[Index];
				const float Output = -Input + BufOut;
				Buffer[Index] = Input + BufOut * Feedback;
				++Index;
				if (Index >= Buffer.size())
				{
					Index = 0;
				}
				return Output;
			}
		};

		// Standard Freeverb delay lengths (samples at 44.1 kHz). Mutually
		// prime so the comb resonances don't align into harsh peaks.
		static constexpr int32_t NumCombs = 8;
		static constexpr int32_t NumAllpasses = 4;
		static constexpr int32_t CombTunings[NumCombs] = {
			1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
		};
		static constexpr int32_t AllpassTunings[NumAllpasses] = {
			556, 441, 341, 225
		};
		// Per-channel offset added to the R bank's delay-line lengths to
		// decorrelate the two reverb tails into a stereo image. The Freeverb
		// reference uses 23 samples at 44.1 kHz; we scale to the device rate
		// in Prepare.
		static constexpr int32_t StereoSpread = 23;

		std::atomic<float> RoomSize{ 0.5f };
		std::atomic<float> Damping{ 0.5f };
		std::atomic<float> Wet{ 0.3f };

		FComb CombsL[NumCombs];
		FComb CombsR[NumCombs];
		FAllpass AllpassesL[NumAllpasses];
		FAllpass AllpassesR[NumAllpasses];

		double SampleRate = 48000.0;
	};
}
