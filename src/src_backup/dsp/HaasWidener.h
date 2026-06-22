#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Haas / precedence-effect widener: add a fixed short delay (5–30 ms) to
	// one channel. Mono input through this becomes apparent stereo via the
	// precedence effect — the brain hears the leading channel as the source
	// direction, but the slight delay broadens the perceived image.
	//
	// Mono compatibility caveat: summing L+R back to mono produces a comb
	// filter at 1/Delay Hz spacing. Acceptable for synth use where pads /
	// leads benefit from the width.
	class FHaasWidener : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_DelayMs,
			Param_Side,    // Choice: which channel is delayed (Right / Left)
			Param_Mix,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "HaasWidener"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to widen." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Width-broadened stereo via fixed-delay precedence effect." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "DelayMs", 0.0f, 30.0f, 15.0f, false, EParamKind::Float, {},
					"Delay applied to the chosen Side. 0 = no widening; 5–25 ms\n"
					"is the precedence-effect window; >30 ms starts to sound like\n"
					"two distinct events." },
				{ "Side",    0.0f, 1.0f,   0.0f, false, EParamKind::Choice,
					{ "Right", "Left" },
					"Which channel gets the delay. The other channel passes through." },
				{ "Mix",     0.0f, 1.0f,   1.0f, false, EParamKind::Float, {},
					"Wet/dry blend. 0 = dry stereo passthrough." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_DelayMs: return DelayMs.load(std::memory_order_relaxed);
				case Param_Side:    return static_cast<float>(SideIdx.load(std::memory_order_relaxed));
				case Param_Mix:     return Mix.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_DelayMs:
				{
					float V = Value;
					if (V < 0.0f)  { V = 0.0f; }
					if (V > 30.0f) { V = 30.0f; }
					DelayMs.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Side:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0) { V = 0; }
					if (V > 1) { V = 1; }
					SideIdx.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
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
			Capacity = static_cast<size_t>(InSampleRate * 0.035) + 8;  // 35 ms + safety
			Buffer.assign(Capacity, 0.0f);
			WriteIdx = 0;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* InL = GetInputBuffer(0, 0);
			const float* InR = GetInputBuffer(0, 1);

			const float DelayMsNow = DelayMs.load(std::memory_order_relaxed);
			const bool bDelayLeft = (SideIdx.load(std::memory_order_relaxed) == 1);
			const float MixNow = Mix.load(std::memory_order_relaxed);

			const float DelaySamples = DelayMsNow * 0.001f * static_cast<float>(SampleRate);
			const int32_t DelayInt = static_cast<int32_t>(DelaySamples);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R = (InR != nullptr) ? InR[I] : L;

				// Tap the delayed source for the chosen side. Non-delayed
				// channel passes the current sample directly.
				const float Source = bDelayLeft ? L : R;
				Buffer[WriteIdx] = Source;
				const int32_t Cap = static_cast<int32_t>(Capacity);
				int32_t ReadIdx = static_cast<int32_t>(WriteIdx) - DelayInt;
				if (ReadIdx < 0) { ReadIdx += Cap; }
				const float Delayed = Buffer[static_cast<size_t>(ReadIdx)];

				const float WetL = bDelayLeft ? Delayed : L;
				const float WetR = bDelayLeft ? R       : Delayed;

				OutL[I] = (1.0f - MixNow) * L + MixNow * WetL;
				OutR[I] = (1.0f - MixNow) * R + MixNow * WetR;

				if (++WriteIdx >= Capacity) { WriteIdx = 0; }
			}
		}

	private:
		std::atomic<float>   DelayMs{ 15.0f };
		std::atomic<uint8_t> SideIdx{ 0 };  // 0 = delay R, 1 = delay L
		std::atomic<float>   Mix{ 1.0f };

		double SampleRate = 48000.0;
		size_t Capacity = 0;
		size_t WriteIdx = 0;
		std::vector<float> Buffer;
	};
}
