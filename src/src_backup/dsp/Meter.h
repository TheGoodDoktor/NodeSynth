#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Audio passthrough that exposes peak and RMS levels to the UI for metering.
	// Peak holds for ~500 ms after each new max, then decays at ~12 dB/sec.
	// RMS is a 50 ms one-pole moving average of sample², sqrt'd at read time.
	class FMeter : public TNodeBase<1, 1>
	{
	public:
		const char* GetTypeName() const override { return "Meter"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to measure." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio, "Pass-through of the input." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return {};  // no user-facing params; the meter is a passive tap
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			HoldSamples = static_cast<uint64_t>(InSampleRate * 0.5);  // 500 ms
			// 12 dB/sec decay → factor per sample = 10^(-12 / (20 * SR)).
			PeakDecayPerSample = std::pow(10.0f,
				-12.0f / (20.0f * static_cast<float>(InSampleRate)));
			// 50 ms one-pole. alpha = 1 - exp(-1 / (SR * 0.05)).
			RmsAlpha = static_cast<float>(
				1.0 - std::exp(-1.0 / (InSampleRate * 0.05)));
			Peak.store(0.0f, std::memory_order_relaxed);
			Rms.store(0.0f, std::memory_order_relaxed);
			InternalPeak = 0.0f;
			InternalRmsSq = 0.0f;
			LastPeakSample = 0;
			SampleCounter = 0;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* AudioIn = GetInputBuffer(0);

			float LocalPeak = InternalPeak;
			float LocalRmsSq = InternalRmsSq;
			uint64_t LocalLastPeak = LastPeakSample;
			uint64_t LocalCounter = SampleCounter;

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float Sample = (AudioIn != nullptr) ? AudioIn[I] : 0.0f;
				Out[I] = Sample;

				const float Abs = std::fabs(Sample);
				if (Abs > LocalPeak)
				{
					LocalPeak = Abs;
					LocalLastPeak = LocalCounter;
				}
				else if (LocalCounter - LocalLastPeak > HoldSamples)
				{
					LocalPeak *= PeakDecayPerSample;
				}

				// One-pole on sample².
				LocalRmsSq += RmsAlpha * (Sample * Sample - LocalRmsSq);
				++LocalCounter;
			}

			InternalPeak = LocalPeak;
			InternalRmsSq = LocalRmsSq;
			LastPeakSample = LocalLastPeak;
			SampleCounter = LocalCounter;

			// Publish to UI atomics once per block.
			Peak.store(LocalPeak, std::memory_order_relaxed);
			Rms.store(std::sqrt(LocalRmsSq), std::memory_order_relaxed);
		}

		// UI accessors.
		float GetPeak() const { return Peak.load(std::memory_order_relaxed); }
		float GetRms() const { return Rms.load(std::memory_order_relaxed); }

	private:
		std::atomic<float> Peak{ 0.0f };
		std::atomic<float> Rms{ 0.0f };

		// Audio-thread state.
		double SampleRate = 48000.0;
		uint64_t HoldSamples = 24000;
		float PeakDecayPerSample = 0.99986f;
		float RmsAlpha = 0.0004f;
		float InternalPeak = 0.0f;
		float InternalRmsSq = 0.0f;
		uint64_t LastPeakSample = 0;
		uint64_t SampleCounter = 0;
	};
}
