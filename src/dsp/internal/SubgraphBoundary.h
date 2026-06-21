#pragma once

#include <string>
#include <vector>

#include "dsp/Node.h"
#include "graph/SubgraphDefinition.h"

namespace NodeSynth::Internal
{
	// The two boundary nodes that surface a subgraph's signature inside its
	// internal editor. They are NOT user-constructible from the palette and
	// NEVER appear in a compiled FAudioGraph — FGraphModel::Compile's expansion
	// pre-pass uses them only to rewire links and then discards them (see
	// docs/PLAN-SUBGRAPHS.md §1.3). Their Process / buffer methods are inert
	// stubs because they're removed before the audio graph runs.
	//
	// Port counts are dynamic: set from the owning definition's pins via
	// SyncSubgraphBoundaries(). Unlike TNodeBase, port count isn't known at
	// compile time, so these derive straight from INode.

	// FSubgraphInputs surfaces the subgraph's INPUT pins as OUTPUT ports —
	// inside the subgraph, the declared inputs are signal sources.
	class FSubgraphInputs : public INode
	{
	public:
		const char* GetTypeName() const override { return "_SubgraphInputs"; }

		void SetPorts(std::vector<FPortInfo> InPorts) { Ports = std::move(InPorts); }

		std::vector<FPortInfo> GetInputPorts() const override { return {}; }
		std::vector<FPortInfo> GetOutputPorts() const override { return Ports; }

		std::shared_ptr<INode> Clone() const override { return nullptr; }

		void Process(const FProcessContext& Ctx) override { (void)Ctx; }

		void SetInputBuffer(uint32_t, const float*, uint32_t = 0) override {}
		const float* GetInputBuffer(uint32_t, uint32_t = 0) const override { return nullptr; }
		float* GetOutputBuffer(uint32_t, uint32_t = 0) override { return nullptr; }

	private:
		std::vector<FPortInfo> Ports;
	};

	// FSubgraphOutputs surfaces the subgraph's OUTPUT pins as INPUT ports —
	// inside the subgraph, the declared outputs are signal sinks.
	class FSubgraphOutputs : public INode
	{
	public:
		const char* GetTypeName() const override { return "_SubgraphOutputs"; }

		void SetPorts(std::vector<FPortInfo> InPorts) { Ports = std::move(InPorts); }

		std::vector<FPortInfo> GetInputPorts() const override { return Ports; }
		std::vector<FPortInfo> GetOutputPorts() const override { return {}; }

		std::shared_ptr<INode> Clone() const override { return nullptr; }

		void Process(const FProcessContext& Ctx) override { (void)Ctx; }

		void SetInputBuffer(uint32_t, const float*, uint32_t = 0) override {}
		const float* GetInputBuffer(uint32_t, uint32_t = 0) const override { return nullptr; }
		float* GetOutputBuffer(uint32_t, uint32_t = 0) override { return nullptr; }

	private:
		std::vector<FPortInfo> Ports;
	};
}

namespace NodeSynth
{
	// Pushes a definition's declared pins onto its boundary nodes' ports, so
	// AddLink validation and the editor see the right signature. Call after
	// constructing / loading a definition and after any pin edit (SG.4).
	inline void SyncSubgraphBoundaries(FSubgraphDefinition& Def)
	{
		auto PinsToPorts = [](const std::vector<FSubgraphPin>& Pins)
		{
			std::vector<FPortInfo> Ports;
			Ports.reserve(Pins.size());
			for (const FSubgraphPin& Pin : Pins)
			{
				Ports.push_back(FPortInfo{ Pin.Name, Pin.Type, Pin.Description });
			}
			return Ports;
		};

		for (const auto& [Id, Rec] : Def.InternalGraph.GetNodes())
		{
			if (!Rec.Node)
			{
				continue;
			}
			if (auto* In = dynamic_cast<Internal::FSubgraphInputs*>(Rec.Node.get()))
			{
				In->SetPorts(PinsToPorts(Def.InputPins));
			}
			else if (auto* Out = dynamic_cast<Internal::FSubgraphOutputs*>(Rec.Node.get()))
			{
				Out->SetPorts(PinsToPorts(Def.OutputPins));
			}
		}
	}

	// Builds a fresh subgraph definition: one Audio input pin, one Audio output
	// pin, the two boundary nodes, and an identity In→Out passthrough link so a
	// freshly created subgraph is immediately valid (audio flows straight
	// through until the user inserts nodes). Used by the editor's "New Subgraph"
	// action; pin editing comes in SG.4.
	inline std::shared_ptr<FSubgraphDefinition> MakeEmptySubgraphDefinition(const std::string& Name)
	{
		auto Def = std::make_shared<FSubgraphDefinition>();
		Def->Name = Name;
		Def->InputPins = { { "In", EPortType::Audio, "" } };
		Def->OutputPins = { { "Out", EPortType::Audio, "" } };

		auto Inputs = std::make_shared<Internal::FSubgraphInputs>();
		auto Outputs = std::make_shared<Internal::FSubgraphOutputs>();
		const FNodeId InId = Def->InternalGraph.AddNode(Inputs, -220.0f, 0.0f);
		const FNodeId OutId = Def->InternalGraph.AddNode(Outputs, 220.0f, 0.0f);

		SyncSubgraphBoundaries(*Def);
		Def->InternalGraph.AddLink(InId, 0, OutId, 0);  // identity passthrough
		return Def;
	}
}
