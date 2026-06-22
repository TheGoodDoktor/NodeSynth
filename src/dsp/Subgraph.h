#pragma once

#include <memory>
#include <vector>

#include "dsp/Node.h"
#include "graph/SubgraphDefinition.h"

namespace NodeSynth
{
	// User-facing subgraph instance. Holds a shared pointer to the definition
	// it instantiates; its ports mirror the definition's declared pins. All
	// instances of the same subgraph in a patch share one FSubgraphDefinition
	// (so editing the definition updates every instance — see §1.8).
	//
	// FSubgraph is macro-expanded away by FGraphModel::Compile before the audio
	// graph is built, so its Process / buffer methods are inert stubs — they
	// never run. The per-voice flag lives on the instance's FNodeRecord and is
	// propagated onto the expanded internal nodes at compile time (§1.6).
	class FSubgraph : public INode
	{
	public:
		const char* GetTypeName() const override { return "Subgraph"; }

		void SetDefinition(std::shared_ptr<FSubgraphDefinition> InDef)
		{
			Definition = std::move(InDef);
		}
		const std::shared_ptr<FSubgraphDefinition>& GetDefinition() const { return Definition; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return Definition ? PinsToPorts(Definition->InputPins) : std::vector<FPortInfo>{};
		}
		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return Definition ? PinsToPorts(Definition->OutputPins) : std::vector<FPortInfo>{};
		}

		// Cloning copies the definition pointer (instances share one definition).
		// Non-null so the per-voice flag can be set on a subgraph instance;
		// expansion runs before per-voice cloning, so this clone is only ever
		// taken via the cloneability probe in FGraphModel::SetNodePerVoice.
		std::shared_ptr<INode> Clone() const override
		{
			auto Copy = std::make_shared<FSubgraph>();
			Copy->Definition = Definition;
			return Copy;
		}

		void Process(const FProcessContext& Ctx) override { (void)Ctx; }

		void SetInputBuffer(uint32_t, const float*, uint32_t = 0) override {}
		const float* GetInputBuffer(uint32_t, uint32_t = 0) const override { return nullptr; }
		float* GetOutputBuffer(uint32_t, uint32_t = 0) override { return nullptr; }

	private:
		static std::vector<FPortInfo> PinsToPorts(const std::vector<FSubgraphPin>& Pins)
		{
			std::vector<FPortInfo> Ports;
			Ports.reserve(Pins.size());
			for (const FSubgraphPin& Pin : Pins)
			{
				Ports.push_back(FPortInfo{ Pin.Name, Pin.Type, Pin.Description });
			}
			return Ports;
		}

		std::shared_ptr<FSubgraphDefinition> Definition;
	};
}
