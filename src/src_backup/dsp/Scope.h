#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Oscilloscope tap. Audio passes straight through so the node can sit
	// anywhere in a chain; the audio thread also writes each sample into a
	// power-of-two ring that the UI thread snapshots once per frame.
	//
	// The ring is single-producer (audio thread writes), single-consumer (UI
	// thread reads). The UI doesn't drain — it just reads the most recent N
	// samples by walking back from the current write index. If the audio
	// thread laps the consumer, we just see a more-recent window — no
	// correctness concern for visualisation.
	class FScope : public TNodeBase<1, 1>
	{
	public:
		static constexpr size_t Capacity = 4096;  // ~85 ms at 48 kHz
		static_assert((Capacity & (Capacity - 1)) == 0,
			"Capacity must be a power of two for the index mask to work");

		enum EParam : uint32_t
		{
			Param_WindowSize,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "Scope"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal to display." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio, "Pass-through of the input." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Window", 32.0f, 2048.0f, 1024.0f, false, EParamKind::Float, {},
					"How many samples the property-panel oscilloscope draws.\n"
					"At 48 kHz, 1024 samples ≈ 21 ms." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			return (Index == Param_WindowSize)
				? static_cast<float>(WindowSize.load(std::memory_order_relaxed))
				: 0.0f;
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			if (Index == Param_WindowSize)
			{
				int32_t V = static_cast<int32_t>(Value);
				if (V < 32) { V = 32; }
				if (V > static_cast<int32_t>(Capacity)) { V = static_cast<int32_t>(Capacity); }
				WindowSize.store(static_cast<uint32_t>(V), std::memory_order_relaxed);
			}
		}

		void Prepare(double /*InSampleRate*/) override
		{
			Buffer.fill(0.0f);
			WriteIndex.store(0, std::memory_order_relaxed);
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			const float* AudioIn = GetInputBuffer(0);

			size_t Idx = WriteIndex.load(std::memory_order_relaxed);
			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float Sample = (AudioIn != nullptr) ? AudioIn[I] : 0.0f;
				Buffer[Idx & (Capacity - 1)] = Sample;
				++Idx;
				Out[I] = Sample;
			}
			WriteIndex.store(Idx, std::memory_order_release);
		}

		// UI-thread accessor: copies the most recent NumSamples samples into Out.
		// Caller-supplied buffer must be at least NumSamples long.
		void Snapshot(float* Out, size_t NumSamples) const
		{
			if (NumSamples > Capacity) { NumSamples = Capacity; }
			const size_t W = WriteIndex.load(std::memory_order_acquire);
			for (size_t I = 0; I < NumSamples; ++I)
			{
				const size_t SourceIdx = (W + Capacity - NumSamples + I) & (Capacity - 1);
				Out[I] = Buffer[SourceIdx];
			}
		}

	private:
		std::atomic<uint32_t> WindowSize{ 1024 };
		std::array<float, Capacity> Buffer{};
		std::atomic<size_t> WriteIndex{ 0 };
	};
}
