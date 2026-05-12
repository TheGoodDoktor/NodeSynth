#pragma once

#include "dsp/Node.h"
#include "dsp/VoiceAllocator.h"

namespace NodeSynth::Internal
{
	// Synthesised by FGraphModel::Compile when a per-voice Audio output feeds
	// a mono Audio input. Sums all NumVoices voice buffers into a single mono
	// audio output. Not user-visible — not in NodeRegistry, not constructible
	// from the palette. Lives only inside compiled FAudioGraph snapshots.
	//
	// Fixed-size at MaxVoices (8). Voices that aren't connected leave their
	// input pointer null and contribute 0, so smaller voice counts (4 / 2 / 1)
	// still produce a mathematically correct sum.
	class FVoiceMixer : public TNodeBase<FVoiceAllocator::MaxVoices, 1>
	{
	public:
		const char* GetTypeName() const override { return "_VoiceMixer"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			std::vector<FPortInfo> Ports;
			Ports.reserve(FVoiceAllocator::MaxVoices);
			for (size_t I = 0; I < FVoiceAllocator::MaxVoices; ++I)
			{
				Ports.push_back({ "V" + std::to_string(I), EPortType::Audio });
			}
			return Ports;
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio } };
		}

		std::shared_ptr<INode> Clone() const override { return nullptr; }

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				float Sum = 0.0f;
				for (size_t V = 0; V < FVoiceAllocator::MaxVoices; ++V)
				{
					const float* In = GetInputBuffer(static_cast<uint32_t>(V));
					if (In != nullptr)
					{
						Sum += In[I];
					}
				}
				Out[I] = Sum;
			}
		}
	};
}
